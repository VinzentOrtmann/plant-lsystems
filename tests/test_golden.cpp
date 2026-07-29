#include "export/VoxWriter.h"
#include "lsystem/LSystem.h"
#include "lsystem/Presets.h"
#include "turtle/Turtle.h"
#include "voxelize/Rasterizer.h"
#include "voxelize/VoxelGrid.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

using namespace plant;

/// End-to-end regression lock.
///
/// Every other test asserts a *property*: that brackets balance, that a signal
/// climbs, that a slab has even thickness. None of them would notice the
/// pipeline quietly producing a slightly different plant -- a tweaked constant,
/// a reordered rotation, an off-by-one in the rasterizer's bounding box. These
/// pin the bytes.
///
/// When one fails, decide whether the change was intended. If it was, the
/// failure message carries the new numbers; paste them in. If it was not, you
/// have just caught a real regression.
namespace {

/// FNV-1a. Hand-rolled because std::hash is implementation-defined and these
/// values are checked into the repository.
class Fnv1a {
public:
    void byte(std::uint8_t value) {
        hash_ ^= value;
        hash_ *= 1099511628211ull;
    }
    void integer(std::int32_t value) {
        const auto bits = static_cast<std::uint32_t>(value);
        for (int shift = 0; shift < 32; shift += 8) {
            byte(static_cast<std::uint8_t>((bits >> shift) & 0xFFu));
        }
    }
    [[nodiscard]] std::uint64_t value() const { return hash_; }

private:
    std::uint64_t hash_ = 1469598103934665603ull;
};

/// Covers geometry, colour and the volume the grid occupies. Deliberately not
/// the float positions: the pipeline's observable output is which voxels are
/// filled, and hashing floats would break on any harmless change of rounding.
std::uint64_t hashGrid(const voxelize::VoxelGrid& grid) {
    Fnv1a hash;
    hash.integer(grid.dimensions().x);
    hash.integer(grid.dimensions().y);
    hash.integer(grid.dimensions().z);
    for (const voxelize::Voxel& voxel : grid.toVector()) {
        hash.integer(voxel.position.x);
        hash.integer(voxel.position.y);
        hash.integer(voxel.position.z);
        hash.byte(voxel.color);
    }
    return hash.value();
}

std::uint64_t hashBytes(const std::vector<std::uint8_t>& bytes) {
    Fnv1a hash;
    for (const std::uint8_t byte : bytes) {
        hash.byte(byte);
    }
    return hash.value();
}

struct Golden {
    const char* preset;
    int iterations;
    std::uint32_t seed;
    int resolution;
    std::size_t segments;
    std::size_t voxels;
    std::uint64_t gridHash;
    std::uint64_t voxFileHash;
};

// Regenerate by running the suite and reading the reported values.
constexpr Golden kGolden[] = {
    {"simple-tree", 12, 1, 64, 2987, 3310, 17782319226457306343ull, 13850420543832329959ull},
    {"fern", 12, 1, 64, 4078, 1668, 6553197162041478021ull, 14525528342756418829ull},
    {"bush", 10, 7, 64, 426, 1573, 1534628028445969298ull, 5097387774797179ull},
    {"signal", 16, 1, 64, 960, 1756, 10382110652006263547ull, 11102250356092446101ull},
    {"leafy", 8, 3, 64, 508, 4759, 10922910856313797595ull, 10044336197159236160ull},
};

voxelize::VoxelGrid buildGrid(const Golden& golden, turtle::Skeleton& skeleton,
                              voxelize::RasterizerConfig& config) {
    const lsystem::GrammarSource* source = lsystem::presets::find(golden.preset);
    REQUIRE(source != nullptr);

    const lsystem::LSystem system = lsystem::LSystem::compile(*source);
    skeleton = turtle::buildSkeleton(system.expand(golden.iterations, golden.seed));
    config.voxelSize = voxelize::voxelSizeForResolution(skeleton, golden.resolution);
    return voxelize::voxelize(skeleton, config);
}

}  // namespace

TEST_CASE("presets produce byte-identical output", "[golden]") {
    for (const Golden& golden : kGolden) {
        CAPTURE(golden.preset);

        turtle::Skeleton skeleton;
        voxelize::RasterizerConfig config;
        const voxelize::VoxelGrid grid = buildGrid(golden, skeleton, config);

        const std::vector<std::uint8_t> encoded = vox::encodeVox(
            grid,
            vox::barkToLeafPalette(config.firstColor, config.colorCount, config.polygonColor));

        const std::uint64_t gridHash = hashGrid(grid);
        const std::uint64_t fileHash = hashBytes(encoded);

        // Reported unconditionally so a failure carries its own replacement.
        INFO("    {\"" << golden.preset << "\", " << golden.iterations << ", " << golden.seed
                       << ", " << golden.resolution << ", " << skeleton.segments.size() << ", "
                       << grid.voxelCount() << ", " << gridHash << "ull, " << fileHash << "ull},");

        CHECK(skeleton.segments.size() == golden.segments);
        CHECK(grid.voxelCount() == golden.voxels);
        CHECK(gridHash == golden.gridHash);
        CHECK(fileHash == golden.voxFileHash);
    }
}

TEST_CASE("every preset is covered by a golden hash", "[golden]") {
    // A new preset without an entry would otherwise sail past this whole file.
    for (const lsystem::GrammarSource& source : lsystem::presets::all()) {
        CAPTURE(source.name);
        bool covered = false;
        for (const Golden& golden : kGolden) {
            covered = covered || source.name == golden.preset;
        }
        CHECK(covered);
    }
}
