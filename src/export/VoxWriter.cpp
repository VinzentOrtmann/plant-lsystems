#include "export/VoxWriter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>

namespace plant::vox {
namespace {

constexpr std::int32_t kVersion = 150;

void appendId(std::vector<std::uint8_t>& out, const char (&id)[5]) {
    out.insert(out.end(), id, id + 4);
}

/// Every integer in the format is little-endian, which is not the same thing as
/// "whatever this machine happens to be".
void appendI32(std::vector<std::uint8_t>& out, std::int32_t value) {
    const auto bits = static_cast<std::uint32_t>(value);
    out.push_back(static_cast<std::uint8_t>(bits & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((bits >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((bits >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((bits >> 24) & 0xFFu));
}

void appendChunk(std::vector<std::uint8_t>& out, const char (&id)[5],
                 const std::vector<std::uint8_t>& content,
                 const std::vector<std::uint8_t>& children) {
    appendId(out, id);
    appendI32(out, static_cast<std::int32_t>(content.size()));
    appendI32(out, static_cast<std::int32_t>(children.size()));
    out.insert(out.end(), content.begin(), content.end());
    out.insert(out.end(), children.begin(), children.end());
}

Rgba lerp(const Rgba& from, const Rgba& to, float t) {
    const auto mix = [t](std::uint8_t a, std::uint8_t b) {
        return static_cast<std::uint8_t>(
            static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t + 0.5f);
    };
    return {mix(from.r, to.r), mix(from.g, to.g), mix(from.b, to.b), 255};
}

}  // namespace

Palette barkToLeafPalette(voxelize::ColorIndex firstColor, int colorCount,
                          voxelize::ColorIndex polygonColor) {
    Palette palette{};
    palette.fill(Rgba{88, 88, 92, 255});
    palette[0] = Rgba{0, 0, 0, 0};  // never drawn

    const Rgba bark{86, 62, 41, 255};
    const Rgba leaf{92, 168, 70, 255};

    const int count = std::max(1, colorCount);
    for (int step = 0; step < count; ++step) {
        const int slot = static_cast<int>(firstColor) + step;
        if (slot < 1 || slot > 255) {
            continue;  // ramp ran off the end of the palette
        }
        const float t = count > 1 ? static_cast<float>(step) / static_cast<float>(count - 1) : 0.0f;
        palette[static_cast<std::size_t>(slot)] = lerp(bark, leaf, t);
    }

    // Foliage sits off the ramp entirely: brighter and cooler than the twig
    // green, so leaves read as leaves rather than as more branch tips.
    if (polygonColor >= 1) {
        palette[polygonColor] = Rgba{116, 196, 84, 255};
    }
    return palette;
}

std::vector<std::uint8_t> encodeVox(const voxelize::VoxelGrid& grid, const Palette& palette) {
    const glm::ivec3 dimensions = grid.dimensions();
    for (int axis = 0; axis < 3; ++axis) {
        if (dimensions[axis] > kMaxDimension) {
            throw VoxWriteError("axis " + std::to_string(axis) + " is " +
                                std::to_string(dimensions[axis]) +
                                " voxels; MagicaVoxel stores coordinates in a single byte, so the "
                                "limit is " +
                                std::to_string(kMaxDimension) + ". Lower the resolution.");
        }
    }

    // MagicaVoxel is Z-up; this project is Y-up with +Z towards the viewer.
    // Mapping (x, y, z) -> (x, depth-1-z, y) swaps the up axis and flips the
    // remaining one, which keeps the determinant positive: mirroring the model
    // instead would turn every left-handed branch spiral into a right-handed one.
    const int width = std::max(1, dimensions.x);
    const int depth = std::max(1, dimensions.z);
    const int height = std::max(1, dimensions.y);

    std::vector<std::uint8_t> size;
    appendI32(size, width);
    appendI32(size, depth);
    appendI32(size, height);

    const std::vector<voxelize::Voxel> voxels = grid.toVector();
    std::vector<std::uint8_t> xyzi;
    xyzi.reserve(4 + voxels.size() * 4);
    appendI32(xyzi, static_cast<std::int32_t>(voxels.size()));
    for (const voxelize::Voxel& voxel : voxels) {
        xyzi.push_back(static_cast<std::uint8_t>(voxel.position.x));
        xyzi.push_back(static_cast<std::uint8_t>(dimensions.z - 1 - voxel.position.z));
        xyzi.push_back(static_cast<std::uint8_t>(voxel.position.y));
        xyzi.push_back(voxel.color);
    }

    // The RGBA chunk is 256 entries, but shifted: the reader does
    // `for (i = 0; i <= 254; i++) palette[i + 1] = ReadRGBA();`, so entry i on
    // disk becomes colour index i + 1 and the last entry is never used.
    std::vector<std::uint8_t> rgba;
    rgba.reserve(1024);
    for (std::size_t index = 1; index <= 255; ++index) {
        rgba.push_back(palette[index].r);
        rgba.push_back(palette[index].g);
        rgba.push_back(palette[index].b);
        rgba.push_back(palette[index].a);
    }
    rgba.insert(rgba.end(), 4, 0);

    std::vector<std::uint8_t> children;
    appendChunk(children, "SIZE", size, {});
    appendChunk(children, "XYZI", xyzi, {});
    appendChunk(children, "RGBA", rgba, {});

    std::vector<std::uint8_t> out;
    out.reserve(children.size() + 32);
    appendId(out, "VOX ");
    appendI32(out, kVersion);
    appendChunk(out, "MAIN", {}, children);
    return out;
}

void writeVox(const std::filesystem::path& path, const voxelize::VoxelGrid& grid,
              const Palette& palette) {
    const std::vector<std::uint8_t> bytes = encodeVox(grid, palette);

    if (path.has_parent_path() && !path.parent_path().empty()) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw VoxWriteError("could not open " + path.string() + " for writing");
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw VoxWriteError("failed while writing " + path.string());
    }
}

}  // namespace plant::vox
