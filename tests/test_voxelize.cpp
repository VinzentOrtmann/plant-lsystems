#include "lsystem/LSystem.h"
#include "lsystem/Presets.h"
#include "turtle/Turtle.h"
#include "voxelize/Rasterizer.h"
#include "voxelize/VoxelGrid.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <vector>

using namespace plant;
using Catch::Matchers::WithinAbs;

namespace {

turtle::Skeleton oneSegment(const glm::vec3& start, const glm::vec3& end, float startRadius,
                            float endRadius) {
    turtle::Segment segment;
    segment.start = start;
    segment.end = end;
    segment.startRadius = startRadius;
    segment.endRadius = endRadius;

    turtle::Skeleton skeleton;
    skeleton.segments.push_back(segment);
    skeleton.boundsMin = glm::min(start - glm::vec3(startRadius), end - glm::vec3(endRadius));
    skeleton.boundsMax = glm::max(start + glm::vec3(startRadius), end + glm::vec3(endRadius));
    return skeleton;
}

void requireVec3(const glm::vec3& actual, const glm::vec3& expected, double tolerance = 1e-5) {
    CHECK_THAT(actual.x, WithinAbs(static_cast<double>(expected.x), tolerance));
    CHECK_THAT(actual.y, WithinAbs(static_cast<double>(expected.y), tolerance));
    CHECK_THAT(actual.z, WithinAbs(static_cast<double>(expected.z), tolerance));
}

turtle::Skeleton presetSkeleton(const lsystem::GrammarSource& source, int iterations) {
    return turtle::buildSkeleton(lsystem::LSystem::compile(source).expand(iterations, 7));
}

/// Size of the largest 26-connected component, found by flood fill. A voxel
/// plant that is not a single component has holes where branches should join.
std::size_t largestComponent(const voxelize::VoxelGrid& grid) {
    const std::vector<voxelize::Voxel> voxels = grid.toVector();
    if (voxels.empty()) {
        return 0;
    }

    std::vector<bool> visited(voxels.size(), false);
    std::vector<std::size_t> lookup;  // index into `voxels` by linear grid index
    const glm::ivec3 dimensions = grid.dimensions();
    lookup.assign(static_cast<std::size_t>(dimensions.x) * static_cast<std::size_t>(dimensions.y) *
                      static_cast<std::size_t>(dimensions.z),
                  static_cast<std::size_t>(-1));
    const auto linear = [&](const glm::ivec3& p) {
        return static_cast<std::size_t>((p.z * dimensions.y + p.y) * dimensions.x + p.x);
    };
    for (std::size_t i = 0; i < voxels.size(); ++i) {
        lookup[linear(voxels[i].position)] = i;
    }

    std::size_t largest = 0;
    std::vector<std::size_t> stack;
    for (std::size_t seed = 0; seed < voxels.size(); ++seed) {
        if (visited[seed]) {
            continue;
        }
        std::size_t component = 0;
        stack.push_back(seed);
        visited[seed] = true;
        while (!stack.empty()) {
            const glm::ivec3 position = voxels[stack.back()].position;
            stack.pop_back();
            ++component;
            for (int dz = -1; dz <= 1; ++dz) {
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const glm::ivec3 neighbour = position + glm::ivec3(dx, dy, dz);
                        if (!grid.inBounds(neighbour)) {
                            continue;
                        }
                        const std::size_t index = lookup[linear(neighbour)];
                        if (index != static_cast<std::size_t>(-1) && !visited[index]) {
                            visited[index] = true;
                            stack.push_back(index);
                        }
                    }
                }
            }
        }
        largest = std::max(largest, component);
    }
    return largest;
}

}  // namespace

// ---------------------------------------------------------------------------
// VoxelGrid
// ---------------------------------------------------------------------------

TEST_CASE("the grid stores and retrieves voxels", "[voxelize][grid]") {
    voxelize::VoxelGrid grid({4, 4, 4}, glm::vec3(0.0f), 1.0f);

    CHECK(grid.empty());
    CHECK(grid.set({1, 2, 3}, 7));
    CHECK(grid.voxelCount() == 1);
    CHECK(grid.at({1, 2, 3}) == 7);
    CHECK(grid.occupied({1, 2, 3}));
    CHECK_FALSE(grid.occupied({0, 0, 0}));

    CHECK(grid.erase({1, 2, 3}));
    CHECK(grid.empty());
    CHECK_FALSE(grid.erase({1, 2, 3}));
}

TEST_CASE("out-of-bounds writes are dropped, not clamped", "[voxelize][grid]") {
    // Clamping would smear geometry onto the walls of the volume; the rasterizer
    // relies on the grid simply refusing.
    voxelize::VoxelGrid grid({4, 4, 4}, glm::vec3(0.0f), 1.0f);

    CHECK_FALSE(grid.set({-1, 0, 0}, 1));
    CHECK_FALSE(grid.set({4, 0, 0}, 1));
    CHECK_FALSE(grid.set({0, 0, 9}, 1));
    CHECK(grid.empty());
    CHECK(grid.at({-1, 0, 0}) == 0);
}

TEST_CASE("colour 0 means empty and is never stored", "[voxelize][grid]") {
    // The .vox format reserves index 0 for air, so a grid that stored it would
    // export voxels that vanish.
    voxelize::VoxelGrid grid({4, 4, 4}, glm::vec3(0.0f), 1.0f);

    CHECK_FALSE(grid.set({1, 1, 1}, 0));
    CHECK(grid.empty());
}

TEST_CASE("index space maps onto world space at voxel centres", "[voxelize][grid]") {
    const voxelize::VoxelGrid grid({8, 8, 8}, glm::vec3(-1.0f, 0.0f, 2.0f), 0.25f);

    const glm::vec3 centre = grid.voxelCenter({0, 0, 0});
    CHECK_THAT(centre.x, WithinAbs(-0.875, 1e-6));
    CHECK_THAT(centre.y, WithinAbs(0.125, 1e-6));
    CHECK_THAT(centre.z, WithinAbs(2.125, 1e-6));

    const glm::vec3 far = grid.voxelCenter({7, 0, 0});
    CHECK_THAT(far.x, WithinAbs(0.875, 1e-6));
}

TEST_CASE("toVector is ordered by z, then y, then x", "[voxelize][grid]") {
    // The hash map's own order is unspecified; the exporter and these tests both
    // need a stable one.
    voxelize::VoxelGrid grid({4, 4, 4}, glm::vec3(0.0f), 1.0f);
    grid.set({3, 0, 1}, 1);
    grid.set({0, 0, 0}, 2);
    grid.set({0, 2, 0}, 3);
    grid.set({1, 0, 0}, 4);

    const std::vector<voxelize::Voxel> ordered = grid.toVector();
    REQUIRE(ordered.size() == 4);
    CHECK(ordered[0].color == 2);
    CHECK(ordered[1].color == 4);
    CHECK(ordered[2].color == 3);
    CHECK(ordered[3].color == 1);
}

TEST_CASE("occupied bounds cover exactly the filled voxels", "[voxelize][grid]") {
    voxelize::VoxelGrid grid({16, 16, 16}, glm::vec3(0.0f), 1.0f);
    glm::ivec3 lo{0};
    glm::ivec3 hi{0};
    CHECK_FALSE(grid.occupiedBounds(lo, hi));

    grid.set({2, 3, 4}, 1);
    grid.set({9, 1, 6}, 1);
    REQUIRE(grid.occupiedBounds(lo, hi));
    CHECK(lo == glm::ivec3(2, 1, 4));
    CHECK(hi == glm::ivec3(9, 3, 6));
}

TEST_CASE("blit copies one grid into another at an offset", "[voxelize][grid]") {
    voxelize::VoxelGrid source({2, 2, 2}, glm::vec3(0.0f), 1.0f);
    source.set({0, 0, 0}, 4);
    source.set({1, 1, 1}, 5);

    voxelize::VoxelGrid destination({8, 8, 8}, glm::vec3(0.0f), 1.0f);
    CHECK(voxelize::blit(destination, source, {3, 0, 2}) == 2);

    CHECK(destination.at({3, 0, 2}) == 4);
    CHECK(destination.at({4, 1, 3}) == 5);
    CHECK(destination.voxelCount() == 2);
    // The source is untouched, and nothing landed at the unshifted position.
    CHECK(source.voxelCount() == 2);
    CHECK(destination.at({0, 0, 0}) == 0);
}

TEST_CASE("blit drops what falls outside instead of clamping", "[voxelize][grid]") {
    // Clamping would smear a specimen against the wall of the sheet rather than
    // simply cropping it.
    voxelize::VoxelGrid source({4, 4, 4}, glm::vec3(0.0f), 1.0f);
    source.set({0, 0, 0}, 1);
    source.set({3, 3, 3}, 2);

    voxelize::VoxelGrid destination({4, 4, 4}, glm::vec3(0.0f), 1.0f);
    CHECK(voxelize::blit(destination, source, {2, 2, 2}) == 1);

    CHECK(destination.at({2, 2, 2}) == 1);
    CHECK(destination.voxelCount() == 1);
}

TEST_CASE("blitting several grids merges them", "[voxelize][grid]") {
    // What a seed sheet does: every specimen voxelized at one shared size, then
    // laid into a single volume.
    voxelize::VoxelGrid specimen({2, 4, 2}, glm::vec3(0.0f), 0.5f);
    specimen.set({1, 3, 1}, 7);

    voxelize::VoxelGrid sheet({8, 4, 8}, glm::vec3(0.0f), 0.5f);
    std::size_t written = 0;
    for (int column = 0; column < 3; ++column) {
        written += voxelize::blit(sheet, specimen, {column * 3, 0, 0});
    }

    CHECK(written == 3);
    CHECK(sheet.voxelCount() == 3);
    CHECK(sheet.at({1, 3, 1}) == 7);
    CHECK(sheet.at({7, 3, 1}) == 7);
}

// ---------------------------------------------------------------------------
// The swept-solid predicate
// ---------------------------------------------------------------------------

TEST_CASE("a round cone with equal radii is a capsule", "[voxelize][geometry]") {
    const glm::vec3 a(0.0f, 0.0f, 0.0f);
    const glm::vec3 b(0.0f, 4.0f, 0.0f);

    CHECK(voxelize::insideRoundCone({0.0f, 2.0f, 0.0f}, a, b, 1.0f, 1.0f));
    CHECK(voxelize::insideRoundCone({0.99f, 2.0f, 0.0f}, a, b, 1.0f, 1.0f));
    CHECK_FALSE(voxelize::insideRoundCone({1.01f, 2.0f, 0.0f}, a, b, 1.0f, 1.0f));

    // Hemispherical caps: the solid reaches a full radius past each endpoint.
    CHECK(voxelize::insideRoundCone({0.0f, -0.99f, 0.0f}, a, b, 1.0f, 1.0f));
    CHECK_FALSE(voxelize::insideRoundCone({0.0f, -1.01f, 0.0f}, a, b, 1.0f, 1.0f));
    CHECK(voxelize::insideRoundCone({0.0f, 4.99f, 0.0f}, a, b, 1.0f, 1.0f));
    CHECK_FALSE(voxelize::insideRoundCone({0.0f, 5.01f, 0.0f}, a, b, 1.0f, 1.0f));
}

TEST_CASE("a tapered cone interpolates its radius along the axis", "[voxelize][geometry]") {
    const glm::vec3 a(0.0f, 0.0f, 0.0f);
    const glm::vec3 b(0.0f, 10.0f, 0.0f);

    // Wide at the base, narrow at the tip.
    CHECK(voxelize::insideRoundCone({1.9f, 0.0f, 0.0f}, a, b, 2.0f, 0.2f));
    CHECK_FALSE(voxelize::insideRoundCone({1.9f, 10.0f, 0.0f}, a, b, 2.0f, 0.2f));
    CHECK(voxelize::insideRoundCone({0.19f, 10.0f, 0.0f}, a, b, 2.0f, 0.2f));

    // Halfway along, the surface sits between the two radii rather than at the
    // naive average -- the swept solid bulges outside the straight-line taper.
    CHECK(voxelize::insideRoundCone({1.1f, 5.0f, 0.0f}, a, b, 2.0f, 0.2f));
}

TEST_CASE("a sphere that swallows the other end still fills", "[voxelize][geometry]") {
    // Degenerate case: the segment is shorter than the radius difference, so
    // the quadratic has no positive leading coefficient.
    const glm::vec3 a(0.0f, 0.0f, 0.0f);
    const glm::vec3 b(0.0f, 0.1f, 0.0f);

    CHECK(voxelize::insideRoundCone({0.0f, 0.0f, 0.9f}, a, b, 1.0f, 0.1f));
    CHECK_FALSE(voxelize::insideRoundCone({0.0f, 0.0f, 1.2f}, a, b, 1.0f, 0.1f));
}

TEST_CASE("the predicate is symmetric under swapping the endpoints", "[voxelize][geometry]") {
    const glm::vec3 a(1.0f, 0.0f, 0.0f);
    const glm::vec3 b(1.0f, 3.0f, 2.0f);

    for (float t = -0.5f; t <= 1.5f; t += 0.1f) {
        for (float offset = 0.0f; offset < 1.5f; offset += 0.25f) {
            const glm::vec3 point = a + t * (b - a) + glm::vec3(offset, 0.0f, 0.0f);
            CHECK(voxelize::insideRoundCone(point, a, b, 0.7f, 0.2f) ==
                  voxelize::insideRoundCone(point, b, a, 0.2f, 0.7f));
        }
    }
}

// ---------------------------------------------------------------------------
// Polygons
// ---------------------------------------------------------------------------

TEST_CASE("closest point on a triangle covers every Voronoi region",
          "[voxelize][geometry][polygon]") {
    const glm::vec3 a(0.0f, 0.0f, 0.0f);
    const glm::vec3 b(4.0f, 0.0f, 0.0f);
    const glm::vec3 c(0.0f, 4.0f, 0.0f);
    const auto nearest = [&](const glm::vec3& p) {
        return voxelize::closestPointOnTriangle(p, a, b, c);
    };

    // Face: straight down onto the interior.
    requireVec3(nearest({1.0f, 1.0f, 5.0f}), {1.0f, 1.0f, 0.0f});
    // Vertices: past a corner in both directions at once.
    requireVec3(nearest({-2.0f, -3.0f, 0.0f}), a);
    requireVec3(nearest({9.0f, -1.0f, 0.0f}), b);
    requireVec3(nearest({-1.0f, 9.0f, 0.0f}), c);
    // Edges: past one edge but between its endpoints.
    requireVec3(nearest({2.0f, -3.0f, 0.0f}), {2.0f, 0.0f, 0.0f});
    requireVec3(nearest({-3.0f, 2.0f, 0.0f}), {0.0f, 2.0f, 0.0f});
    requireVec3(nearest({4.0f, 4.0f, 0.0f}), {2.0f, 2.0f, 0.0f});
}

TEST_CASE("a degenerate triangle does not divide by zero", "[voxelize][geometry][polygon]") {
    const glm::vec3 a(1.0f, 1.0f, 1.0f);
    const glm::vec3 result = voxelize::closestPointOnTriangle({5.0f, 5.0f, 5.0f}, a, a, a);
    CHECK(std::isfinite(result.x));
    CHECK(std::isfinite(result.y));
}

TEST_CASE("a polygon fills a slab of even thickness", "[voxelize][polygon]") {
    // A flat square in the XZ plane: the filled volume should be its area times
    // the configured thickness, not a wedge that thins towards the edges.
    turtle::Skeleton skeleton;
    skeleton.polygons.push_back({{{0.0f, 0.0f, 0.0f},
                                  {2.0f, 0.0f, 0.0f},
                                  {2.0f, 0.0f, 2.0f},
                                  {0.0f, 0.0f, 2.0f}},
                                 0});
    skeleton.boundsMin = {0.0f, 0.0f, 0.0f};
    skeleton.boundsMax = {2.0f, 0.0f, 2.0f};

    voxelize::RasterizerConfig config;
    config.voxelSize = 0.05f;
    config.polygonThicknessVoxels = 2.0f;

    const voxelize::VoxelGrid grid = voxelize::voxelize(skeleton, config);
    const double filled = static_cast<double>(grid.voxelCount()) *
                          std::pow(static_cast<double>(config.voxelSize), 3.0);
    const double expected = 2.0 * 2.0 * (2.0 * config.voxelSize);

    CHECK(filled > expected * 0.9);
    CHECK(filled < expected * 1.25);  // rounded edges add a little
}

TEST_CASE("polygons take the foliage colour, branches the depth ramp", "[voxelize][polygon]") {
    turtle::Skeleton skeleton = oneSegment({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.05f, 0.05f);
    skeleton.polygons.push_back({{{0.5f, 1.0f, 0.0f},
                                  {1.0f, 1.5f, 0.0f},
                                  {0.5f, 2.0f, 0.0f}},
                                 1});
    skeleton.boundsMax = {1.0f, 2.0f, 0.05f};

    voxelize::RasterizerConfig config;
    config.voxelSize = 0.04f;

    const voxelize::VoxelGrid grid = voxelize::voxelize(skeleton, config);
    std::size_t branchVoxels = 0;
    std::size_t leafVoxels = 0;
    for (const voxelize::Voxel& voxel : grid.toVector()) {
        if (voxel.color == config.polygonColor) {
            ++leafVoxels;
        } else {
            ++branchVoxels;
        }
    }
    CHECK(branchVoxels > 0);
    CHECK(leafVoxels > 0);
}

TEST_CASE("a skeleton of nothing but polygons still voxelizes", "[voxelize][polygon]") {
    // The grid used to be sized from segment radii alone, which would leave a
    // leaf-only skeleton with no volume at all.
    turtle::Skeleton skeleton;
    skeleton.polygons.push_back({{{0.0f, 0.0f, 0.0f},
                                  {1.0f, 0.0f, 0.0f},
                                  {0.5f, 1.0f, 0.0f}},
                                 0});
    skeleton.boundsMin = {0.0f, 0.0f, 0.0f};
    skeleton.boundsMax = {1.0f, 1.0f, 0.0f};

    voxelize::RasterizerConfig config;
    config.voxelSize = 0.05f;

    const voxelize::VoxelGrid grid = voxelize::voxelize(skeleton, config);
    CHECK(grid.voxelCount() > 100);
    CHECK(grid.dimensions().z >= 3);  // padding plus the slab itself
}

// ---------------------------------------------------------------------------
// Rasterization
// ---------------------------------------------------------------------------

TEST_CASE("a vertical segment rasterizes into a column of the right height", "[voxelize]") {
    voxelize::RasterizerConfig config;
    config.voxelSize = 0.1f;

    const turtle::Skeleton skeleton =
        oneSegment({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.05f, 0.05f);
    const voxelize::VoxelGrid grid = voxelize::voxelize(skeleton, config);

    REQUIRE_FALSE(grid.empty());
    glm::ivec3 lo{0};
    glm::ivec3 hi{0};
    REQUIRE(grid.occupiedBounds(lo, hi));

    // A one-unit segment at a tenth of a unit per voxel, plus the rounded caps.
    const int height = hi.y - lo.y + 1;
    CHECK(height >= 10);
    CHECK(height <= 14);
    // Thin: the radius floor is under a voxel, so the column stays narrow.
    CHECK(hi.x - lo.x <= 2);
    CHECK(hi.z - lo.z <= 2);
}

TEST_CASE("voxel count scales with the volume of the solid", "[voxelize]") {
    // Doubling the radius of a long cylinder should roughly quadruple its
    // voxels; a rasterizer that leaked into the bounding box instead would
    // scale with the cube.
    voxelize::RasterizerConfig config;
    config.voxelSize = 0.02f;

    const std::size_t thin = voxelize::voxelize(
        oneSegment({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.1f, 0.1f), config).voxelCount();
    const std::size_t thick = voxelize::voxelize(
        oneSegment({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.2f, 0.2f), config).voxelCount();

    const double ratio = static_cast<double>(thick) / static_cast<double>(thin);
    CHECK(ratio > 3.2);
    CHECK(ratio < 4.8);
}

TEST_CASE("the filled volume is close to the analytic cylinder volume", "[voxelize]") {
    voxelize::RasterizerConfig config;
    config.voxelSize = 0.01f;

    const float radius = 0.15f;
    const float length = 1.0f;
    const voxelize::VoxelGrid grid = voxelize::voxelize(
        oneSegment({0.0f, 0.0f, 0.0f}, {0.0f, length, 0.0f}, radius, radius), config);

    // Cylinder plus the two hemispherical caps, i.e. a capsule.
    const double expected = 3.14159265 * radius * radius * length +
                            4.0 / 3.0 * 3.14159265 * radius * radius * radius;
    const double actual = static_cast<double>(grid.voxelCount()) *
                          std::pow(static_cast<double>(config.voxelSize), 3.0);

    CHECK(actual > expected * 0.97);
    CHECK(actual < expected * 1.03);
}

TEST_CASE("thin twigs still rasterize to a connected trail", "[voxelize]") {
    // A radius well under a voxel would sample to a dotted line without the
    // minRadiusVoxels floor.
    voxelize::RasterizerConfig config;
    config.voxelSize = 0.1f;

    const voxelize::VoxelGrid grid = voxelize::voxelize(
        oneSegment({0.0f, 0.0f, 0.0f}, {0.7f, 1.0f, 0.4f}, 0.001f, 0.001f), config);

    REQUIRE(grid.voxelCount() > 5);
    CHECK(largestComponent(grid) == grid.voxelCount());
}

TEST_CASE("branch depth selects a colour from the ramp", "[voxelize]") {
    voxelize::RasterizerConfig config;
    config.voxelSize = 0.1f;
    config.firstColor = 10;
    config.colorCount = 4;

    turtle::Skeleton skeleton = oneSegment({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.05f, 0.05f);
    turtle::Segment tip = skeleton.segments.front();
    tip.start = {0.0f, 1.0f, 0.0f};
    tip.end = {0.0f, 2.0f, 0.0f};
    tip.depth = 3;
    skeleton.segments.push_back(tip);
    skeleton.boundsMax.y = 2.05f;

    const voxelize::VoxelGrid grid = voxelize::voxelize(skeleton, config);
    std::vector<voxelize::ColorIndex> seen;
    for (const voxelize::Voxel& voxel : grid.toVector()) {
        if (std::find(seen.begin(), seen.end(), voxel.color) == seen.end()) {
            seen.push_back(voxel.color);
        }
    }
    std::sort(seen.begin(), seen.end());

    REQUIRE(seen.size() == 2);
    CHECK(seen.front() == 10);  // depth 0 -> first slot
    CHECK(seen.back() == 13);   // depth 3 of 3 -> last slot
}

TEST_CASE("an empty skeleton voxelizes to an empty grid", "[voxelize]") {
    const voxelize::VoxelGrid grid = voxelize::voxelize(turtle::Skeleton{});

    CHECK(grid.empty());
    CHECK(grid.dimensions() == glm::ivec3(0));
}

TEST_CASE("a voxel size that would blow up the grid is refused", "[voxelize][errors]") {
    voxelize::RasterizerConfig config;
    config.voxelSize = 0.0005f;
    config.maxDimension = 64;

    CHECK_THROWS_AS(
        voxelize::voxelize(oneSegment({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 0.05f, 0.05f), config),
        voxelize::VoxelizeError);

    config.voxelSize = 0.0f;
    CHECK_THROWS_AS(voxelize::voxelize(turtle::Skeleton{}, config), voxelize::VoxelizeError);
}

TEST_CASE("voxelSizeForResolution hits the requested grid size", "[voxelize]") {
    const turtle::Skeleton skeleton = presetSkeleton(lsystem::presets::simpleTree(), 10);

    voxelize::RasterizerConfig config;
    config.voxelSize = voxelize::voxelSizeForResolution(skeleton, 96);
    const voxelize::VoxelGrid grid = voxelize::voxelize(skeleton, config);

    const glm::ivec3 dimensions = grid.dimensions();
    const int longest = std::max({dimensions.x, dimensions.y, dimensions.z});
    // The requested count, plus a padding voxel and up to a minimum-radius
    // voxel on each side, plus rounding.
    CHECK(longest >= 96);
    CHECK(longest <= 102);

    CHECK(voxelize::voxelSizeForResolution(turtle::Skeleton{}, 64) == 1.0f);
}

// ---------------------------------------------------------------------------
// Presets end to end
// ---------------------------------------------------------------------------

TEST_CASE("every preset voxelizes into one connected solid", "[voxelize][presets]") {
    // A plant that falls into pieces exports as a cloud of floating fragments.
    // Segments share endpoints and radii, so their swept spheres coincide there
    // and the joints should close without any special handling.
    for (const lsystem::GrammarSource& source : lsystem::presets::all()) {
        CAPTURE(source.name);

        const turtle::Skeleton skeleton = presetSkeleton(source, 10);
        voxelize::RasterizerConfig config;
        config.voxelSize = voxelize::voxelSizeForResolution(skeleton, 100);

        const voxelize::VoxelGrid grid = voxelize::voxelize(skeleton, config);

        REQUIRE(grid.voxelCount() > 500);
        CHECK(largestComponent(grid) == grid.voxelCount());
    }
}

TEST_CASE("voxelization stays inside the grid it allocated", "[voxelize][presets]") {
    const turtle::Skeleton skeleton = presetSkeleton(lsystem::presets::fern(), 10);
    voxelize::RasterizerConfig config;
    config.voxelSize = voxelize::voxelSizeForResolution(skeleton, 80);

    const voxelize::VoxelGrid grid = voxelize::voxelize(skeleton, config);

    glm::ivec3 lo{0};
    glm::ivec3 hi{0};
    REQUIRE(grid.occupiedBounds(lo, hi));
    // Nothing was silently clipped against the walls: the padding voxel on each
    // side stays empty.
    CHECK(lo.x >= 1);
    CHECK(lo.y >= 1);
    CHECK(lo.z >= 1);
    CHECK(hi.x <= grid.dimensions().x - 2);
    CHECK(hi.y <= grid.dimensions().y - 2);
    CHECK(hi.z <= grid.dimensions().z - 2);
}

TEST_CASE("voxelization is deterministic", "[voxelize][presets]") {
    const turtle::Skeleton skeleton = presetSkeleton(lsystem::presets::bush(), 9);
    voxelize::RasterizerConfig config;
    config.voxelSize = voxelize::voxelSizeForResolution(skeleton, 64);

    const std::vector<voxelize::Voxel> first = voxelize::voxelize(skeleton, config).toVector();
    const std::vector<voxelize::Voxel> second = voxelize::voxelize(skeleton, config).toVector();

    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        CHECK(first[i].position == second[i].position);
        CHECK(first[i].color == second[i].color);
    }
}
