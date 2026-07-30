#include "colonize/Colonize.h"
#include "turtle/Turtle.h"
#include "voxelize/Metrics.h"
#include "voxelize/Rasterizer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace plant;
using Catch::Matchers::WithinAbs;

namespace {

/// Small enough to keep the suite quick, large enough to actually branch.
colonize::ColonizeConfig quickConfig() {
    colonize::ColonizeConfig config;
    config.attractorCount = 250;
    config.crownCentre = {0.0f, 1.6f, 0.0f};
    config.crownRadii = {0.8f, 0.9f, 0.8f};
    config.influenceRadius = 0.6f;
    config.killDistance = 0.14f;
    config.stepLength = 0.09f;
    config.maxIterations = 200;
    return config;
}

std::string shapeOf(const turtle::Skeleton& skeleton) {
    std::string out;
    for (const turtle::Segment& segment : skeleton.segments) {
        out += std::to_string(static_cast<int>(segment.end.x * 1000.0f)) + ',' +
               std::to_string(static_cast<int>(segment.end.y * 1000.0f)) + ',' +
               std::to_string(static_cast<int>(segment.end.z * 1000.0f)) + ';';
    }
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Growth
// ---------------------------------------------------------------------------

TEST_CASE("a tree grows towards its attraction cloud", "[colonize]") {
    colonize::ColonizeStats stats;
    const turtle::Skeleton skeleton = colonize::grow(quickConfig(), 7, &stats);

    REQUIRE(skeleton.segments.size() > 50);
    CHECK(stats.nodes == skeleton.segments.size() + 1);  // one node has no incoming segment
    CHECK(stats.iterations > 0);

    // Most of the cloud gets reached; a few points in awkward corners will not.
    CHECK(stats.attractorsReached > stats.attractorsTotal * 3 / 4);

    // It starts at the origin and ends up inside the crown.
    CHECK_THAT(skeleton.segments.front().start.y, WithinAbs(0.0, 1e-5));
    CHECK(skeleton.boundsMax.y > 1.5f);
}

TEST_CASE("a seed grows exactly one tree", "[colonize]") {
    const colonize::ColonizeConfig config = quickConfig();

    CHECK(shapeOf(colonize::grow(config, 99)) == shapeOf(colonize::grow(config, 99)));
    CHECK(shapeOf(colonize::grow(config, 99)) != shapeOf(colonize::grow(config, 100)));
}

TEST_CASE("the crown shape decides the tree shape", "[colonize]") {
    // The whole difference from an L-system: no rule changes, only the volume
    // the branches are reaching into.
    colonize::ColonizeConfig wide = quickConfig();
    wide.crownRadii = {1.4f, 0.5f, 1.4f};
    colonize::ColonizeConfig tall = quickConfig();
    tall.crownRadii = {0.4f, 1.5f, 0.4f};

    const turtle::Skeleton flat = colonize::grow(wide, 3);
    const turtle::Skeleton column = colonize::grow(tall, 3);

    const glm::vec3 flatSize = flat.size();
    const glm::vec3 columnSize = column.size();
    CHECK(flatSize.x > columnSize.x * 1.5f);
    CHECK(columnSize.y > flatSize.y * 1.2f);
}

TEST_CASE("the trunk reaches a crown placed far above the root", "[colonize]") {
    // Without the trunk phase nothing is ever in reach and the tree is a single
    // node -- the classic way to get an empty result from this algorithm.
    colonize::ColonizeConfig high = quickConfig();
    high.crownCentre = {0.0f, 4.0f, 0.0f};

    colonize::ColonizeStats stats;
    const turtle::Skeleton skeleton = colonize::grow(high, 5, &stats);

    REQUIRE(skeleton.segments.size() > 50);
    CHECK(skeleton.boundsMax.y > 3.5f);
    CHECK(stats.attractorsReached > 0);
}

// ---------------------------------------------------------------------------
// Invariants the rest of the pipeline relies on
// ---------------------------------------------------------------------------

TEST_CASE("a parent segment always precedes its child", "[colonize]") {
    // The voxelizer and the taper pass both walk the list in order.
    const turtle::Skeleton skeleton = colonize::grow(quickConfig(), 11);

    REQUIRE_FALSE(skeleton.segments.empty());
    for (std::size_t i = 0; i < skeleton.segments.size(); ++i) {
        CHECK(skeleton.segments[i].parent < static_cast<int>(i));
    }
}

TEST_CASE("segments are connected, positive length and positive radius", "[colonize]") {
    const turtle::Skeleton skeleton = colonize::grow(quickConfig(), 13);

    for (const turtle::Segment& segment : skeleton.segments) {
        CHECK(segment.length() > 0.0f);
        CHECK(segment.startRadius > 0.0f);
        CHECK(segment.endRadius > 0.0f);
        CHECK(std::isfinite(segment.end.x));
        CHECK(std::isfinite(segment.end.y));
        CHECK(std::isfinite(segment.end.z));
        if (segment.parent >= 0) {
            // A child starts exactly where its parent ended.
            const glm::vec3 gap =
                segment.start - skeleton.segments[static_cast<std::size_t>(segment.parent)].end;
            CHECK(glm::length(gap) < 1e-5f);
        }
    }
}

TEST_CASE("bounds cover the segments including their radii", "[colonize]") {
    const turtle::Skeleton skeleton = colonize::grow(quickConfig(), 17);

    for (const turtle::Segment& segment : skeleton.segments) {
        CHECK(segment.end.y - segment.endRadius >= skeleton.boundsMin.y - 1e-4f);
        CHECK(segment.end.y + segment.endRadius <= skeleton.boundsMax.y + 1e-4f);
    }
}

// ---------------------------------------------------------------------------
// Thickness
// ---------------------------------------------------------------------------

TEST_CASE("branches thicken towards the trunk", "[colonize][radii]") {
    const turtle::Skeleton skeleton = colonize::grow(quickConfig(), 23);

    // Radii live on nodes, and a segment's end radius is its distal node's, so a
    // parent's end radius equals each child's start radius by construction. The
    // claim with content is about the far ends: a parent is at least as thick as
    // any single branch above it, because it carries all of them.
    for (const turtle::Segment& segment : skeleton.segments) {
        if (segment.parent >= 0) {
            const turtle::Segment& parent =
                skeleton.segments[static_cast<std::size_t>(segment.parent)];
            CHECK(parent.endRadius >= segment.endRadius - 1e-5f);
            CHECK_THAT(static_cast<double>(parent.endRadius),
                       WithinAbs(static_cast<double>(segment.startRadius), 1e-6));
        }
    }
    // And the trunk is the thickest thing in the tree.
    CHECK_THAT(skeleton.segments.front().startRadius, WithinAbs(skeleton.maxRadius(), 1e-5));
}

TEST_CASE("the pipe model relates a node to everything above it", "[colonize][radii]") {
    // r^n at a node equals its own length plus the sum of r^n over the nodes
    // directly above it. The own-length term is what Murray's law on tip counts
    // lacks, and its absence is what renders a trunk as a uniform tube.
    colonize::ColonizeConfig config = quickConfig();
    config.taperExponent = 2.2f;
    // The floor is switched off for this one. It has to be: clamping a twig
    // *upwards* can leave an unclamped parent thinner than the sum of its
    // clamped children, so the identity only holds for the model itself.
    config.tipRadius = 0.0f;
    const turtle::Skeleton skeleton = colonize::grow(config, 29);
    const double n = 2.2;

    std::vector<double> carried(skeleton.segments.size(), 0.0);
    std::vector<int> children(skeleton.segments.size(), 0);
    for (const turtle::Segment& segment : skeleton.segments) {
        if (segment.parent >= 0) {
            const auto p = static_cast<std::size_t>(segment.parent);
            carried[p] += std::pow(static_cast<double>(segment.endRadius), n);
            children[p]++;
        }
    }

    int forksChecked = 0;
    for (std::size_t i = 0; i < skeleton.segments.size(); ++i) {
        if (children[i] < 2) {
            continue;
        }
        // Above a fork the children account for everything except the fork
        // segment's own length, so the parent must be strictly thicker.
        ++forksChecked;
        const double childrenOnly = std::pow(carried[i], 1.0 / n);
        const double actual = static_cast<double>(skeleton.segments[i].endRadius);
        CHECK(actual >= childrenOnly - 1e-6);
        CHECK(actual < childrenOnly * 1.5);  // and not wildly thicker
    }
    CHECK(forksChecked > 5);  // the tree really does fork
}

TEST_CASE("the trunk lands on the requested radius", "[colonize][radii]") {
    // Solving the taper rule alone gives an absolute answer that depends on how
    // many twigs happened to grow, which is not something a user should have to
    // compensate for. Radii are scaled so the root hits trunkRadius exactly.
    for (const float wanted : {0.02f, 0.055f, 0.15f}) {
        colonize::ColonizeConfig config = quickConfig();
        config.trunkRadius = wanted;
        CAPTURE(wanted);
        CHECK_THAT(colonize::grow(config, 31).segments.front().startRadius,
                   WithinAbs(static_cast<double>(wanted), 1e-4));
    }
}

TEST_CASE("an unbranched bole narrows rather than staying constant",
          "[colonize][radii]") {
    // The difference from Murray's law on tip counts, which is *exactly* constant
    // along a chain. Accumulating length instead makes it strictly decreasing.
    //
    // Only strictly, though, and worth being clear about: nearly all the
    // accumulated length lives in the crown, so over a short bole the change is
    // a fraction of a percent. What actually fixed the fat uniform trunk in the
    // render was pinning the root to trunkRadius, not this.
    colonize::ColonizeConfig high = quickConfig();
    high.crownCentre = {0.0f, 3.0f, 0.0f};
    high.tipRadius = 0.0f;
    const turtle::Skeleton skeleton = colonize::grow(high, 5);

    REQUIRE(skeleton.segments.size() > 20);
    std::vector<int> children(skeleton.segments.size(), 0);
    for (const turtle::Segment& segment : skeleton.segments) {
        if (segment.parent >= 0) {
            children[static_cast<std::size_t>(segment.parent)]++;
        }
    }
    std::size_t last = 0;
    while (last + 1 < skeleton.segments.size() && children[last] == 1) {
        ++last;
    }
    REQUIRE(last > 4);  // there is a real bole to measure

    // Monotonically narrowing all the way up it.
    for (std::size_t i = 1; i <= last; ++i) {
        CHECK(skeleton.segments[i].startRadius <= skeleton.segments[i - 1].startRadius);
    }
    CHECK(skeleton.segments[last].startRadius < skeleton.segments[0].startRadius);
}

TEST_CASE("a larger exponent tapers more gently", "[colonize][radii]") {
    // The root is pinned to trunkRadius either way, so the exponent only shows
    // up in how fast thickness falls off above it -- and it falls off *slower*
    // as n rises, because r/rRoot = (acc/accRoot)^(1/n) and a ratio below one
    // raised to a smaller power sits closer to one.
    const auto midFraction = [](float exponent) {
        colonize::ColonizeConfig config = quickConfig();
        config.taperExponent = exponent;
        config.tipRadius = 0.0f;
        const turtle::Skeleton skeleton = colonize::grow(config, 31);
        const turtle::Segment& mid = skeleton.segments[skeleton.segments.size() / 8];
        return mid.startRadius / skeleton.segments.front().startRadius;
    };

    const float gentle = midFraction(3.5f);
    const float steep = midFraction(1.5f);
    CAPTURE(gentle, steep);
    CHECK(steep < gentle);
}

// ---------------------------------------------------------------------------
// Degenerate configurations
// ---------------------------------------------------------------------------

TEST_CASE("no attraction points means no growth, not a crash", "[colonize][errors]") {
    colonize::ColonizeConfig empty = quickConfig();
    empty.attractorCount = 0;

    colonize::ColonizeStats stats;
    const turtle::Skeleton skeleton = colonize::grow(empty, 3, &stats);

    CHECK(skeleton.segments.empty());
    CHECK(stats.attractorsTotal == 0);
    CHECK(skeleton.boundsMin == skeleton.boundsMax);
}

TEST_CASE("a kill distance wider than the influence radius terminates", "[colonize][errors]") {
    // Points are consumed the instant they pull, so growth stops almost at
    // once. It must stop rather than spin.
    colonize::ColonizeConfig greedy = quickConfig();
    greedy.killDistance = 5.0f;

    colonize::ColonizeStats stats;
    CHECK_NOTHROW(colonize::grow(greedy, 3, &stats));
    CHECK(stats.iterations < greedy.maxIterations);
}

TEST_CASE("zero iterations yields only the trunk", "[colonize][errors]") {
    colonize::ColonizeConfig frozen = quickConfig();
    frozen.maxIterations = 0;

    const turtle::Skeleton skeleton = colonize::grow(frozen, 3);
    // The trunk phase still runs, and it stops as soon as the crown is in reach.
    CHECK(skeleton.segments.size() < 40);
    for (const turtle::Segment& segment : skeleton.segments) {
        // A bare trunk: every segment continues the one before it.
        CHECK(segment.depth == 0);
    }
}

TEST_CASE("degenerate distances do not hang", "[colonize][errors]") {
    colonize::ColonizeConfig odd = quickConfig();
    odd.influenceRadius = 0.0f;
    odd.stepLength = 0.0f;
    odd.killDistance = 0.0f;
    odd.maxIterations = 50;

    CHECK_NOTHROW(colonize::grow(odd, 3));
}

// ---------------------------------------------------------------------------
// Downstream
// ---------------------------------------------------------------------------

TEST_CASE("a colonized tree feeds the rest of the pipeline unchanged",
          "[colonize][integration]") {
    // The point of producing a Skeleton: the voxelizer, the metrics and
    // everything past them need no knowledge of where it came from.
    const turtle::Skeleton skeleton = colonize::grow(quickConfig(), 41);

    voxelize::RasterizerConfig config;
    config.voxelSize = voxelize::voxelSizeForResolution(skeleton, 96);
    const voxelize::VoxelGrid grid = voxelize::voxelize(skeleton, config);

    CHECK(grid.voxelCount() > 1000);

    const double dimension = voxelize::boxCountingDimension(grid);
    CAPTURE(dimension);
    CHECK(dimension > 1.0);
    CHECK(dimension < 3.0);
}
