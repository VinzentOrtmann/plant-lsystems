#include "export/VoxWriter.h"
#include "lsystem/LSystem.h"
#include "lsystem/Presets.h"
#include "turtle/Turtle.h"
#include "voxelize/Rasterizer.h"
#include "voxelize/VoxelGrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace plant;

namespace {

/// Minimal .vox reader used only by these tests. Parsing the bytes back is the
/// only way to check the writer against the format rather than against itself.
struct VoxFile {
    std::int32_t version = 0;
    glm::ivec3 size{0};
    struct Entry {
        std::uint8_t x = 0;
        std::uint8_t y = 0;
        std::uint8_t z = 0;
        std::uint8_t color = 0;
    };
    std::vector<Entry> voxels;
    std::vector<vox::Rgba> palette;  // as stored on disk, 256 entries
    std::map<std::string, int> chunkCounts;
};

std::int32_t readI32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    REQUIRE(offset + 4 <= bytes.size());
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(bytes[offset]) |
                                     (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
                                     (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
                                     (static_cast<std::uint32_t>(bytes[offset + 3]) << 24));
}

void parseChunks(const std::vector<std::uint8_t>& bytes, std::size_t begin, std::size_t end,
                 VoxFile& file) {
    std::size_t offset = begin;
    while (offset + 12 <= end) {
        const std::string id(reinterpret_cast<const char*>(&bytes[offset]), 4);
        const auto contentBytes = static_cast<std::size_t>(readI32(bytes, offset + 4));
        const auto childBytes = static_cast<std::size_t>(readI32(bytes, offset + 8));
        const std::size_t contentBegin = offset + 12;
        const std::size_t childBegin = contentBegin + contentBytes;
        REQUIRE(childBegin + childBytes <= end);

        ++file.chunkCounts[id];

        if (id == "SIZE") {
            REQUIRE(contentBytes == 12);
            file.size = {readI32(bytes, contentBegin), readI32(bytes, contentBegin + 4),
                         readI32(bytes, contentBegin + 8)};
        } else if (id == "XYZI") {
            const auto count = static_cast<std::size_t>(readI32(bytes, contentBegin));
            REQUIRE(contentBytes == 4 + count * 4);
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t at = contentBegin + 4 + i * 4;
                file.voxels.push_back({bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]});
            }
        } else if (id == "RGBA") {
            REQUIRE(contentBytes == 1024);
            for (std::size_t i = 0; i < 256; ++i) {
                const std::size_t at = contentBegin + i * 4;
                file.palette.push_back({bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]});
            }
        }

        parseChunks(bytes, childBegin, childBegin + childBytes, file);
        offset = childBegin + childBytes;
    }
    REQUIRE(offset == end);
}

VoxFile parseVox(const std::vector<std::uint8_t>& bytes) {
    REQUIRE(bytes.size() >= 8);
    REQUIRE(std::memcmp(bytes.data(), "VOX ", 4) == 0);

    VoxFile file;
    file.version = readI32(bytes, 4);
    parseChunks(bytes, 8, bytes.size(), file);
    return file;
}

voxelize::VoxelGrid tinyGrid() {
    voxelize::VoxelGrid grid({4, 5, 6}, glm::vec3(0.0f), 1.0f);
    grid.set({1, 2, 3}, 9);
    return grid;
}

turtle::Skeleton presetSkeleton(const lsystem::GrammarSource& source, int iterations) {
    return turtle::buildSkeleton(lsystem::LSystem::compile(source).expand(iterations, 5));
}

std::filesystem::path scratchFile(const std::string& name) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "plant-lsystems-tests";
    std::filesystem::create_directories(directory);
    return directory / name;
}

}  // namespace

// ---------------------------------------------------------------------------
// Container structure
// ---------------------------------------------------------------------------

TEST_CASE("the file starts with the VOX magic and version 150", "[vox]") {
    const std::vector<std::uint8_t> bytes =
        vox::encodeVox(tinyGrid(), vox::barkToLeafPalette());

    REQUIRE(bytes.size() > 8);
    CHECK(std::memcmp(bytes.data(), "VOX ", 4) == 0);
    CHECK(parseVox(bytes).version == 150);
}

TEST_CASE("chunk sizes account for every byte in the file", "[vox]") {
    // parseVox walks the chunk tree and REQUIREs each level consumes exactly the
    // range its parent declared, so a wrong content or child length fails here.
    const std::vector<std::uint8_t> bytes =
        vox::encodeVox(tinyGrid(), vox::barkToLeafPalette());
    const VoxFile file = parseVox(bytes);

    CHECK(file.chunkCounts.at("MAIN") == 1);
    CHECK(file.chunkCounts.at("SIZE") == 1);
    CHECK(file.chunkCounts.at("XYZI") == 1);
    CHECK(file.chunkCounts.at("RGBA") == 1);
}

TEST_CASE("MAIN carries no content of its own", "[vox]") {
    const std::vector<std::uint8_t> bytes =
        vox::encodeVox(tinyGrid(), vox::barkToLeafPalette());

    // "VOX " + version, then MAIN's id and its two lengths.
    CHECK(std::memcmp(&bytes[8], "MAIN", 4) == 0);
    CHECK(readI32(bytes, 12) == 0);
    CHECK(readI32(bytes, 16) == static_cast<std::int32_t>(bytes.size() - 20));
}

// ---------------------------------------------------------------------------
// Coordinates
// ---------------------------------------------------------------------------

TEST_CASE("the up axis moves from Y to Z", "[vox][coordinates]") {
    // MagicaVoxel is Z-up; this project is Y-up. A grid that is 4 wide, 5 tall
    // and 6 deep becomes 4 wide, 6 deep and 5 tall on disk.
    const VoxFile file = parseVox(vox::encodeVox(tinyGrid(), vox::barkToLeafPalette()));

    CHECK(file.size == glm::ivec3(4, 6, 5));
}

TEST_CASE("a voxel lands where the axis swap puts it", "[vox][coordinates]") {
    const VoxFile file = parseVox(vox::encodeVox(tinyGrid(), vox::barkToLeafPalette()));

    REQUIRE(file.voxels.size() == 1);
    // Grid (1,2,3) in a 4x5x6 volume: x passes through, the depth axis is
    // flipped (6-1-3 = 2), and the old height becomes the new up axis.
    CHECK(file.voxels[0].x == 1);
    CHECK(file.voxels[0].y == 2);
    CHECK(file.voxels[0].z == 2);
    CHECK(file.voxels[0].color == 9);
}

TEST_CASE("the axis swap preserves handedness", "[vox][coordinates]") {
    // Mirroring instead of rotating would turn every left-handed branch spiral
    // into a right-handed one. Check the mapping's determinant directly: three
    // unit steps along grid X, Y and Z must still form a right-handed basis
    // once transformed.
    voxelize::VoxelGrid grid({8, 8, 8}, glm::vec3(0.0f), 1.0f);
    grid.set({0, 0, 0}, 1);
    grid.set({1, 0, 0}, 2);  // +X
    grid.set({0, 1, 0}, 3);  // +Y (up)
    grid.set({0, 0, 1}, 4);  // +Z (towards the viewer)

    const VoxFile file = parseVox(vox::encodeVox(grid, vox::barkToLeafPalette()));
    REQUIRE(file.voxels.size() == 4);

    std::map<int, glm::ivec3> byColor;
    for (const VoxFile::Entry& entry : file.voxels) {
        byColor[entry.color] = {entry.x, entry.y, entry.z};
    }
    const glm::ivec3 origin = byColor.at(1);
    const glm::ivec3 dx = byColor.at(2) - origin;
    const glm::ivec3 dy = byColor.at(3) - origin;
    const glm::ivec3 dz = byColor.at(4) - origin;

    CHECK(dx == glm::ivec3(1, 0, 0));   // right stays right
    CHECK(dy == glm::ivec3(0, 0, 1));   // up becomes the new Z
    CHECK(dz == glm::ivec3(0, -1, 0));  // towards the viewer becomes -Y

    const int determinant = dx.x * (dy.y * dz.z - dy.z * dz.y) -
                            dx.y * (dy.x * dz.z - dy.z * dz.x) +
                            dx.z * (dy.x * dz.y - dy.y * dz.x);
    CHECK(determinant == 1);  // +1 rotates, -1 would mirror
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------

TEST_CASE("the palette chunk is shifted down by one slot", "[vox][palette]") {
    // The format's own reader does `for (i = 0; i <= 254; i++) palette[i+1] =
    // ReadRGBA()`, so colour index 1 has to be the first entry on disk. Writing
    // it unshifted would tint the whole model by one slot.
    vox::Palette palette = vox::barkToLeafPalette();
    palette[1] = {10, 20, 30, 255};
    palette[255] = {40, 50, 60, 255};

    const VoxFile file = parseVox(vox::encodeVox(tinyGrid(), palette));

    REQUIRE(file.palette.size() == 256);
    CHECK(file.palette[0].r == 10);
    CHECK(file.palette[0].g == 20);
    CHECK(file.palette[0].b == 30);
    CHECK(file.palette[254].r == 40);
    CHECK(file.palette[254].g == 50);
    CHECK(file.palette[254].b == 60);
}

TEST_CASE("the ramp runs from bark to leaf across its slots", "[vox][palette]") {
    const vox::Palette palette = vox::barkToLeafPalette(1, 8);

    // Ends are the two anchor colours...
    CHECK(palette[1].r == 86);
    CHECK(palette[1].g == 62);
    CHECK(palette[8].r == 92);
    CHECK(palette[8].g == 168);

    // ...and green rises monotonically in between.
    for (std::size_t slot = 1; slot < 8; ++slot) {
        CHECK(palette[slot].g <= palette[slot + 1].g);
    }
    // Slot 0 is never drawn, and slots outside both the ramp and the foliage
    // colour are neutral.
    CHECK(palette[0].a == 0);
    CHECK(palette[20].r == palette[20].g);
}

TEST_CASE("foliage gets its own slot off the ramp", "[vox][palette]") {
    // Leaves have to read as leaves, not as more twig tips, so the polygon
    // colour sits outside the bark-to-leaf ramp rather than at the end of it.
    const vox::Palette palette = vox::barkToLeafPalette(1, 8, 9);

    CHECK(palette[9].g > palette[8].g);
    CHECK(palette[9].r != palette[9].g);
}

TEST_CASE("a ramp that runs off the end of the palette is clipped, not wrapped",
          "[vox][palette]") {
    const vox::Palette palette = vox::barkToLeafPalette(252, 8);

    CHECK(palette[252].r == 86);
    CHECK(palette[255].r != 88);  // still inside the ramp
    CHECK(palette[1].r == 88);    // untouched neutral, not wrapped onto
}

// ---------------------------------------------------------------------------
// Errors and edges
// ---------------------------------------------------------------------------

TEST_CASE("a grid past the format's byte-sized axis limit is refused", "[vox][errors]") {
    const voxelize::VoxelGrid oversized({257, 4, 4}, glm::vec3(0.0f), 1.0f);

    CHECK_THROWS_AS(vox::encodeVox(oversized, vox::barkToLeafPalette()), vox::VoxWriteError);

    // Exactly at the limit is fine: coordinates 0..255 fit in a byte.
    const voxelize::VoxelGrid atLimit({256, 4, 4}, glm::vec3(0.0f), 1.0f);
    CHECK_NOTHROW(vox::encodeVox(atLimit, vox::barkToLeafPalette()));
}

TEST_CASE("a voxel at the far corner survives the byte round trip", "[vox][errors]") {
    voxelize::VoxelGrid grid({256, 256, 256}, glm::vec3(0.0f), 1.0f);
    grid.set({255, 255, 0}, 3);

    const VoxFile file = parseVox(vox::encodeVox(grid, vox::barkToLeafPalette()));
    REQUIRE(file.voxels.size() == 1);
    CHECK(file.voxels[0].x == 255);
    CHECK(file.voxels[0].y == 255);
    CHECK(file.voxels[0].z == 255);
}

TEST_CASE("an empty grid still produces a structurally valid file", "[vox][errors]") {
    const std::vector<std::uint8_t> bytes =
        vox::encodeVox(voxelize::VoxelGrid{}, vox::barkToLeafPalette());
    const VoxFile file = parseVox(bytes);

    CHECK(file.voxels.empty());
    // Zero-sized axes would be a malformed model; the writer floors them at 1.
    CHECK(file.size == glm::ivec3(1, 1, 1));
}

// ---------------------------------------------------------------------------
// File output
// ---------------------------------------------------------------------------

TEST_CASE("writeVox puts exactly the encoded bytes on disk", "[vox][io]") {
    const voxelize::VoxelGrid grid = tinyGrid();
    const vox::Palette palette = vox::barkToLeafPalette();
    const std::filesystem::path path = scratchFile("tiny.vox");

    vox::writeVox(path, grid, palette);
    REQUIRE(std::filesystem::exists(path));

    std::vector<std::uint8_t> onDisk;
    {
        // Scoped: Windows refuses to remove a file that still has an open handle.
        std::ifstream file(path, std::ios::binary);
        onDisk.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    CHECK(onDisk == vox::encodeVox(grid, palette));

    std::filesystem::remove(path);
}

TEST_CASE("writeVox creates missing parent directories", "[vox][io]") {
    const std::filesystem::path path = scratchFile("nested") / "deeper" / "plant.vox";
    std::filesystem::remove_all(scratchFile("nested"));

    CHECK_NOTHROW(vox::writeVox(path, tinyGrid(), vox::barkToLeafPalette()));
    CHECK(std::filesystem::exists(path));

    std::filesystem::remove_all(scratchFile("nested"));
}

// ---------------------------------------------------------------------------
// End to end
// ---------------------------------------------------------------------------

TEST_CASE("a real plant survives the whole pipeline into a .vox file", "[vox][presets]") {
    for (const lsystem::GrammarSource& source : lsystem::presets::all()) {
        CAPTURE(source.name);

        const turtle::Skeleton skeleton = presetSkeleton(source, 10);
        voxelize::RasterizerConfig config;
        config.voxelSize = voxelize::voxelSizeForResolution(skeleton, 100);
        const voxelize::VoxelGrid grid = voxelize::voxelize(skeleton, config);

        const VoxFile file = parseVox(vox::encodeVox(
            grid,
            vox::barkToLeafPalette(config.firstColor, config.colorCount, config.polygonColor)));

        REQUIRE(file.voxels.size() == grid.voxelCount());
        CHECK(file.size.x == grid.dimensions().x);
        CHECK(file.size.y == grid.dimensions().z);
        CHECK(file.size.z == grid.dimensions().y);

        for (const VoxFile::Entry& entry : file.voxels) {
            // Every voxel is inside the declared volume and carries a drawable
            // colour: index 0 means empty and would silently vanish.
            CHECK(entry.x < file.size.x);
            CHECK(entry.y < file.size.y);
            CHECK(entry.z < file.size.z);
            CHECK(entry.color != 0);
            // Either somewhere on the branch ramp, or the foliage slot.
            const bool onRamp = entry.color >= config.firstColor &&
                                entry.color < config.firstColor + config.colorCount;
            CHECK((onRamp || entry.color == config.polygonColor));
        }
    }
}
