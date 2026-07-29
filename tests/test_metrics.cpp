#include "lsystem/LSystem.h"
#include "lsystem/Presets.h"
#include "turtle/Turtle.h"
#include "voxelize/Metrics.h"
#include "voxelize/Rasterizer.h"
#include "voxelize/VoxelGrid.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace plant;
using Catch::Matchers::WithinAbs;

namespace {

/// Every voxel in a box of `size` cubed.
voxelize::VoxelGrid solidCube(int size) {
    voxelize::VoxelGrid grid({size, size, size}, glm::vec3(0.0f), 1.0f);
    for (int z = 0; z < size; ++z) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                grid.set({x, y, z}, 1);
            }
        }
    }
    return grid;
}

voxelize::VoxelGrid straightLine(int length) {
    voxelize::VoxelGrid grid({length, length, length}, glm::vec3(0.0f), 1.0f);
    for (int x = 0; x < length; ++x) {
        grid.set({x, 0, 0}, 1);
    }
    return grid;
}

voxelize::VoxelGrid flatSheet(int size) {
    voxelize::VoxelGrid grid({size, size, size}, glm::vec3(0.0f), 1.0f);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            grid.set({x, y, 0}, 1);
        }
    }
    return grid;
}

voxelize::VoxelGrid presetGrid(const lsystem::GrammarSource& source, int iterations,
                               int resolution) {
    const turtle::Skeleton skeleton =
        turtle::buildSkeleton(lsystem::LSystem::compile(source).expand(iterations, 5));
    voxelize::RasterizerConfig config;
    config.voxelSize = voxelize::voxelSizeForResolution(skeleton, resolution);
    return voxelize::voxelize(skeleton, config);
}

}  // namespace

// ---------------------------------------------------------------------------
// Box counting
// ---------------------------------------------------------------------------

TEST_CASE("box counts halve the resolution each step", "[metrics]") {
    const std::vector<voxelize::BoxCount> counts = voxelize::boxCounts(solidCube(16));

    REQUIRE(counts.size() == 5);  // 1, 2, 4, 8, 16
    CHECK(counts[0].boxSize == 1);
    CHECK(counts[0].boxes == 16 * 16 * 16);
    CHECK(counts[1].boxSize == 2);
    CHECK(counts[1].boxes == 8 * 8 * 8);
    CHECK(counts[2].boxes == 4 * 4 * 4);
    CHECK(counts[3].boxes == 2 * 2 * 2);
    CHECK(counts[4].boxes == 1);
}

TEST_CASE("an empty grid has no counts and no dimension", "[metrics]") {
    const voxelize::VoxelGrid empty;
    CHECK(voxelize::boxCounts(empty).empty());
    CHECK(voxelize::boxCountingDimension(empty) == 0.0);
}

TEST_CASE("a model too small to have a scaling range reports nothing", "[metrics]") {
    // Better than a confidently wrong number from two noisy points.
    voxelize::VoxelGrid tiny({4, 4, 4}, glm::vec3(0.0f), 1.0f);
    tiny.set({1, 1, 1}, 1);
    tiny.set({2, 2, 2}, 1);

    CHECK(voxelize::boxCountingDimension(tiny) == 0.0);
}

// ---------------------------------------------------------------------------
// Shapes of known dimension
// ---------------------------------------------------------------------------

TEST_CASE("familiar shapes measure their Euclidean dimension", "[metrics]") {
    // The calibration that makes any reading on a plant meaningful. These are
    // exact scalings, so the fit should land essentially on the integer.
    SECTION("a solid cube is three-dimensional") {
        CHECK_THAT(voxelize::boxCountingDimension(solidCube(64)), WithinAbs(3.0, 0.02));
    }
    SECTION("a flat sheet is two-dimensional") {
        CHECK_THAT(voxelize::boxCountingDimension(flatSheet(64)), WithinAbs(2.0, 0.02));
    }
    SECTION("a straight line is one-dimensional") {
        CHECK_THAT(voxelize::boxCountingDimension(straightLine(64)), WithinAbs(1.0, 0.02));
    }
}

TEST_CASE("dimension is independent of where the model sits in the grid", "[metrics]") {
    // Anchoring the boxes to the grid rather than the model would make the same
    // shape measure differently depending on how the padding placed it: at
    // coarse sizes it straddles a different number of cell boundaries. Anchored
    // to the model, a shift changes nothing at all.
    voxelize::VoxelGrid shifted({80, 80, 80}, glm::vec3(0.0f), 1.0f);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            shifted.set({x + 7, y + 5, 3}, 1);
        }
    }
    CHECK_THAT(voxelize::boxCountingDimension(shifted), WithinAbs(2.0, 0.02));
    CHECK(voxelize::boxCountingDimension(shifted) ==
          voxelize::boxCountingDimension(flatSheet(64)));
}

TEST_CASE("a Menger-like sponge measures between two and three", "[metrics]") {
    // One subdivision of the classic construction: a 3x3x3 block with the
    // centre of each face and the core removed, tiled. Its exact dimension is
    // log(20)/log(3) = 2.727, which is the kind of value only a real fractal
    // gives -- a good check that the fit is not simply rounding to integers.
    constexpr int kLevels = 4;
    constexpr int kSize = 81;  // 3^4
    voxelize::VoxelGrid sponge({kSize, kSize, kSize}, glm::vec3(0.0f), 1.0f);

    const auto inSponge = [](int x, int y, int z) {
        for (int level = 0; level < kLevels; ++level) {
            const int cx = x % 3;
            const int cy = y % 3;
            const int cz = z % 3;
            // Removed when the cell is central on two or more axes.
            const int centres = (cx == 1 ? 1 : 0) + (cy == 1 ? 1 : 0) + (cz == 1 ? 1 : 0);
            if (centres >= 2) {
                return false;
            }
            x /= 3;
            y /= 3;
            z /= 3;
        }
        return true;
    };

    for (int z = 0; z < kSize; ++z) {
        for (int y = 0; y < kSize; ++y) {
            for (int x = 0; x < kSize; ++x) {
                if (inSponge(x, y, z)) {
                    sponge.set({x, y, z}, 1);
                }
            }
        }
    }

    const double dimension = voxelize::boxCountingDimension(sponge);
    CHECK(dimension > 2.4);
    CHECK(dimension < 3.0);
}

// ---------------------------------------------------------------------------
// Plants
// ---------------------------------------------------------------------------

TEST_CASE("every preset measures somewhere between a line and a solid", "[metrics][presets]") {
    for (const lsystem::GrammarSource& source : lsystem::presets::all()) {
        CAPTURE(source.name);
        const double dimension =
            voxelize::boxCountingDimension(presetGrid(source, 12, 128));

        CHECK(dimension > 1.0);
        CHECK(dimension < 3.0);
    }
}

TEST_CASE("foliage fills space more densely than bare branches", "[metrics][presets]") {
    // The measurement earning its keep: leafy and simple-tree branch similarly,
    // but one of them ends every twig in a filled surface, and the dimension
    // should say so.
    const double leafy = voxelize::boxCountingDimension(
        presetGrid(lsystem::presets::leafyShoot(), 10, 128));
    const double bare = voxelize::boxCountingDimension(
        presetGrid(lsystem::presets::simpleTree(), 10, 128));

    CAPTURE(leafy, bare);
    CHECK(leafy > bare);
}

TEST_CASE("the measurement is deterministic", "[metrics][presets]") {
    const voxelize::VoxelGrid grid = presetGrid(lsystem::presets::bush(), 10, 96);
    CHECK(voxelize::boxCountingDimension(grid) == voxelize::boxCountingDimension(grid));
}
