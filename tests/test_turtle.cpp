#include "lsystem/LSystem.h"
#include "lsystem/Presets.h"
#include "turtle/Turtle.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <cmath>
#include <limits>
#include <string>

using namespace plant;
using Catch::Matchers::WithinAbs;

namespace {

/// Builds a word from source syntax by borrowing the grammar parser's axiom
/// handling: `word("F(1,0.1)[+(90)F(1)]")` reads far better in a test than a
/// hand-assembled vector of modules.
lsystem::Word word(const std::string& text) {
    lsystem::GrammarSource source;
    source.name = "test";
    source.axiom = text;
    return lsystem::LSystem::compile(source).axiom();
}

void requireVec3(const glm::vec3& actual, const glm::vec3& expected, double tolerance = 1e-5);

void requireVec3(const glm::vec3& actual, const glm::vec3& expected, double tolerance) {
    CHECK_THAT(actual.x, WithinAbs(static_cast<double>(expected.x), tolerance));
    CHECK_THAT(actual.y, WithinAbs(static_cast<double>(expected.y), tolerance));
    CHECK_THAT(actual.z, WithinAbs(static_cast<double>(expected.z), tolerance));
}

/// Heading of the single segment a word produces, which is how every rotation
/// test below observes the turtle's frame.
glm::vec3 headingAfter(const std::string& text) {
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word(text + "F(1)"));
    REQUIRE(skeleton.segments.size() == 1);
    return skeleton.segments.front().direction();
}

}  // namespace

// ---------------------------------------------------------------------------
// Movement and frame
// ---------------------------------------------------------------------------

TEST_CASE("F draws a segment along the heading and advances the turtle", "[turtle]") {
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("F(2,0.1)F(1,0.1)"));

    REQUIRE(skeleton.segments.size() == 2);
    requireVec3(skeleton.segments[0].start, glm::vec3(0.0f, 0.0f, 0.0f));
    requireVec3(skeleton.segments[0].end, glm::vec3(0.0f, 2.0f, 0.0f));
    requireVec3(skeleton.segments[1].start, glm::vec3(0.0f, 2.0f, 0.0f));
    requireVec3(skeleton.segments[1].end, glm::vec3(0.0f, 3.0f, 0.0f));

    CHECK_THAT(skeleton.segments[0].length(), WithinAbs(2.0, 1e-5));
    CHECK(skeleton.segments[0].startRadius == 0.1f);
}

TEST_CASE("the turtle starts pointing up", "[turtle]") {
    requireVec3(headingAfter(""), glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_CASE("rotation symbols turn about the expected local axes", "[turtle]") {
    // The frame is H = +Y, L = -X, U = +Z. Each quarter turn below lands the
    // heading on another axis, so a swapped or negated rotation is unmissable.
    SECTION("yaw turns about up") {
        requireVec3(headingAfter("+(90)"), glm::vec3(-1.0f, 0.0f, 0.0f));  // towards L
        requireVec3(headingAfter("-(90)"), glm::vec3(1.0f, 0.0f, 0.0f));
    }
    SECTION("pitch turns about left") {
        requireVec3(headingAfter("&(90)"), glm::vec3(0.0f, 0.0f, -1.0f));  // towards -U
        requireVec3(headingAfter("^(90)"), glm::vec3(0.0f, 0.0f, 1.0f));
    }
    SECTION("roll leaves the heading alone") {
        requireVec3(headingAfter("\\(90)"), glm::vec3(0.0f, 1.0f, 0.0f));
        requireVec3(headingAfter("/(137.5)"), glm::vec3(0.0f, 1.0f, 0.0f));
    }
}

TEST_CASE("roll reorients the frame that later turns use", "[turtle]") {
    // Roll leaves the heading alone but carries U with it, so the following yaw
    // sweeps through a different world plane. This is the whole reason 3D
    // L-systems need roll: without it every yaw stays in one plane forever.
    requireVec3(headingAfter("\\(90)+(90)"), glm::vec3(0.0f, 0.0f, 1.0f));
    requireVec3(headingAfter("/(90)+(90)"), glm::vec3(0.0f, 0.0f, -1.0f));
}

TEST_CASE("$ rolls upright without disturbing the heading", "[turtle]") {
    // Pitch over so the turtle is horizontal, roll it into an arbitrary twist,
    // then level it: the heading must survive untouched while U returns to
    // world up.
    const turtle::Skeleton rolled = turtle::buildSkeleton(word("&(90)/(50)$F(1)+(90)F(1)"));

    REQUIRE(rolled.segments.size() == 2);
    // &(90) points the turtle at -Z; the roll and the levelling leave that alone.
    requireVec3(rolled.segments[0].direction(), glm::vec3(0.0f, 0.0f, -1.0f));
    // With U back at world up, a quarter turn about it is purely horizontal.
    requireVec3(rolled.segments[1].direction(), glm::vec3(-1.0f, 0.0f, 0.0f));
}

TEST_CASE("$ leaves an already-vertical turtle alone", "[turtle]") {
    // The world-up cross product degenerates when the heading is vertical;
    // the turtle has to keep its existing frame rather than produce NaNs.
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("$F(1)"));

    REQUIRE(skeleton.segments.size() == 1);
    requireVec3(skeleton.segments.front().direction(), glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_CASE("opposite rotations cancel", "[turtle]") {
    requireVec3(headingAfter("+(37)-(37)"), glm::vec3(0.0f, 1.0f, 0.0f));
    requireVec3(headingAfter("&(37)^(37)"), glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_CASE("rotations without a parameter fall back to the configured angle", "[turtle]") {
    turtle::TurtleConfig config;
    config.defaultAngle = 90.0f;

    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("+F(1)"), config);
    REQUIRE(skeleton.segments.size() == 1);
    requireVec3(skeleton.segments.front().direction(), glm::vec3(-1.0f, 0.0f, 0.0f));
}

TEST_CASE("angleScale sweeps every rotation without touching the grammar", "[turtle]") {
    turtle::TurtleConfig config;
    config.angleScale = 0.0f;  // straight up regardless of what the word says

    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("+(90)F(1)"), config);
    requireVec3(skeleton.segments.front().direction(), glm::vec3(0.0f, 1.0f, 0.0f));

    config.angleScale = 2.0f;  // 45 degrees becomes 90
    const turtle::Skeleton doubled = turtle::buildSkeleton(word("+(45)F(1)"), config);
    requireVec3(doubled.segments.front().direction(), glm::vec3(-1.0f, 0.0f, 0.0f));
}

// ---------------------------------------------------------------------------
// Tropism
// ---------------------------------------------------------------------------

TEST_CASE("tropism bends the heading towards its vector by |H x T| radians", "[turtle][tropism]") {
    turtle::TurtleConfig config;
    config.tropism = {0.0f, -1.0f, 0.0f};  // gravity
    config.tropismStrength = 0.1f;

    // +(90) points the turtle along -X, square to gravity, so |H x T| is 1 and
    // the bend is the full strength: a tenth of a radian per segment.
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("+(90)F(1)F(1)F(1)"), config);
    REQUIRE(skeleton.segments.size() == 3);

    // The first segment is drawn before any bending.
    requireVec3(skeleton.segments[0].direction(), glm::vec3(-1.0f, 0.0f, 0.0f));
    requireVec3(skeleton.segments[1].direction(),
                glm::vec3(-std::cos(0.1f), -std::sin(0.1f), 0.0f));

    // Bending accumulates, but each step is weaker than the last: |H x T| is
    // the sine of the angle still to close, so it decays as the heading swings
    // towards the tropism and the branch asymptotes instead of spinning past it.
    const float second = 0.1f + 0.1f * std::cos(0.1f);
    CHECK(second < 0.2f);
    requireVec3(skeleton.segments[2].direction(),
                glm::vec3(-std::cos(second), -std::sin(second), 0.0f));
}

TEST_CASE("tropism never bends past its own vector", "[turtle][tropism]") {
    // The decay above has to hold all the way: a long horizontal chain under
    // gravity should settle pointing straight down, not oscillate around it.
    turtle::TurtleConfig config;
    config.tropismStrength = 0.4f;

    std::string chain = "+(90)";
    for (int i = 0; i < 200; ++i) {
        chain += "F(0.05)";
    }
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word(chain), config);

    REQUIRE(skeleton.segments.size() == 200);
    const glm::vec3 settled = skeleton.segments.back().direction();
    requireVec3(settled, glm::vec3(0.0f, -1.0f, 0.0f), 1e-3);

    // Monotone: every segment points at least as far down as the one before it.
    for (std::size_t i = 1; i < skeleton.segments.size(); ++i) {
        CHECK(skeleton.segments[i].direction().y <= skeleton.segments[i - 1].direction().y + 1e-5f);
    }
}

TEST_CASE("a heading aligned with the tropism is left alone", "[turtle][tropism]") {
    // |H x T| is zero for a vertical trunk under gravity, which is what keeps
    // trunks straight while limbs sag.
    turtle::TurtleConfig config;
    config.tropismStrength = 0.3f;

    const turtle::Skeleton down = turtle::buildSkeleton(word("&(90)&(90)F(1)F(1)"), config);
    REQUIRE(down.segments.size() == 2);
    requireVec3(down.segments[1].direction(), down.segments[0].direction());

    const turtle::Skeleton up = turtle::buildSkeleton(word("F(1)F(1)"), config);
    REQUIRE(up.segments.size() == 2);
    requireVec3(up.segments[1].direction(), glm::vec3(0.0f, 1.0f, 0.0f));
}

TEST_CASE("negative strength bends away from the tropism", "[turtle][tropism]") {
    // Same knob, opposite sign: gravity becomes phototropism.
    turtle::TurtleConfig config;
    config.tropismStrength = -0.1f;

    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("+(90)F(1)F(1)"), config);
    REQUIRE(skeleton.segments.size() == 2);
    CHECK(skeleton.segments[1].direction().y > 0.0f);
}

TEST_CASE("zero strength leaves the skeleton untouched", "[turtle][tropism]") {
    const lsystem::Word expanded =
        lsystem::LSystem::compile(lsystem::presets::simpleTree()).expand(8);

    turtle::TurtleConfig off;
    off.tropismStrength = 0.0f;
    const turtle::Skeleton without = turtle::buildSkeleton(expanded, off);
    const turtle::Skeleton reference = turtle::buildSkeleton(expanded);

    REQUIRE(without.segments.size() == reference.segments.size());
    for (std::size_t i = 0; i < without.segments.size(); ++i) {
        requireVec3(without.segments[i].end, reference.segments[i].end);
    }
}

TEST_CASE("a degenerate tropism vector is ignored rather than producing NaNs",
          "[turtle][tropism]") {
    turtle::TurtleConfig config;
    config.tropism = {0.0f, 0.0f, 0.0f};
    config.tropismStrength = 0.5f;

    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("+(45)F(1)F(1)"), config);
    REQUIRE(skeleton.segments.size() == 2);
    CHECK(std::isfinite(skeleton.segments[1].end.x));
    CHECK(std::isfinite(skeleton.segments[1].end.y));
    CHECK(std::isfinite(skeleton.segments[1].end.z));
}

TEST_CASE("gravity drags a whole tree downwards", "[turtle][tropism][presets]") {
    const lsystem::Word expanded =
        lsystem::LSystem::compile(lsystem::presets::simpleTree()).expand(10);

    turtle::TurtleConfig config;
    config.tropismStrength = 0.12f;

    const turtle::Skeleton straight = turtle::buildSkeleton(expanded);
    const turtle::Skeleton drooping = turtle::buildSkeleton(expanded, config);

    REQUIRE(straight.segments.size() == drooping.segments.size());

    // Not measured on the bounding box: the top of this tree is set by its
    // vertical leader and the bottom by the base radius, and neither of those
    // bends, so the bounds come out bit-identical while every lateral sags.
    const auto meanTipHeight = [](const turtle::Skeleton& skeleton) {
        double total = 0.0;
        for (const turtle::Segment& segment : skeleton.segments) {
            total += static_cast<double>(segment.end.y);
        }
        return total / static_cast<double>(skeleton.segments.size());
    };
    CHECK(meanTipHeight(drooping) < meanTipHeight(straight) - 0.05);

    // The leader is square to gravity's cross product, so it holds its line.
    requireVec3(drooping.segments.front().direction(), straight.segments.front().direction());
}

TEST_CASE("f moves without leaving a segment behind", "[turtle]") {
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("f(3)F(1,0.1)"));

    REQUIRE(skeleton.segments.size() == 1);
    requireVec3(skeleton.segments.front().start, glm::vec3(0.0f, 3.0f, 0.0f));
}

TEST_CASE("zero-length segments are dropped rather than becoming degenerate cylinders",
          "[turtle]") {
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("F(0,0.1)F(1,0.1)"));

    REQUIRE(skeleton.segments.size() == 1);
    requireVec3(skeleton.segments.front().start, glm::vec3(0.0f, 0.0f, 0.0f));
}

// ---------------------------------------------------------------------------
// Branching
// ---------------------------------------------------------------------------

TEST_CASE("brackets save and restore the whole turtle state", "[turtle]") {
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("F(1,0.1)[+(90)F(1,0.05)]F(1,0.1)"));

    REQUIRE(skeleton.segments.size() == 3);
    // The branch starts where the trunk ended and goes sideways...
    requireVec3(skeleton.segments[1].start, glm::vec3(0.0f, 1.0f, 0.0f));
    requireVec3(skeleton.segments[1].end, glm::vec3(-1.0f, 1.0f, 0.0f));
    // ...and the trunk resumes as if the branch had never happened.
    requireVec3(skeleton.segments[2].start, glm::vec3(0.0f, 1.0f, 0.0f));
    requireVec3(skeleton.segments[2].end, glm::vec3(0.0f, 2.0f, 0.0f));
}

TEST_CASE("segments record their parent and branch depth", "[turtle]") {
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("F(1,0.1)[+(30)F(1,0.05)[F(1,0.02)]]F(1,0.1)"));

    REQUIRE(skeleton.segments.size() == 4);
    CHECK(skeleton.segments[0].parent == -1);
    CHECK(skeleton.segments[1].parent == 0);
    CHECK(skeleton.segments[2].parent == 1);
    CHECK(skeleton.segments[3].parent == 0);

    CHECK(skeleton.segments[0].depth == 0);
    CHECK(skeleton.segments[1].depth == 1);
    CHECK(skeleton.segments[2].depth == 2);
    CHECK(skeleton.segments[3].depth == 0);
    CHECK(skeleton.maxDepth() == 2);
}

TEST_CASE("a parent always precedes its child", "[turtle]") {
    // The voxelizer and the taper pass both walk the segment list in order and
    // rely on this.
    const lsystem::LSystem system = lsystem::LSystem::compile(lsystem::presets::simpleTree());
    const turtle::Skeleton skeleton = turtle::buildSkeleton(system.expand(10));

    REQUIRE(skeleton.segments.size() > 100);
    for (std::size_t i = 0; i < skeleton.segments.size(); ++i) {
        CHECK(skeleton.segments[i].parent < static_cast<int>(i));
    }
}

TEST_CASE("unbalanced brackets are reported", "[turtle][errors]") {
    CHECK_THROWS_AS(turtle::buildSkeleton(word("F(1)]")), turtle::TurtleError);
    CHECK_THROWS_AS(turtle::buildSkeleton(word("[F(1)")), turtle::TurtleError);
}

TEST_CASE("symbols the turtle does not know are skipped", "[turtle]") {
    // Buds survive in a word that was not expanded to completion; they must not
    // disturb the geometry.
    const turtle::Skeleton withBuds = turtle::buildSkeleton(word("F(1,0.1)A(3,4)F(1,0.1)B"));
    const turtle::Skeleton without = turtle::buildSkeleton(word("F(1,0.1)F(1,0.1)"));

    REQUIRE(withBuds.segments.size() == without.segments.size());
    requireVec3(withBuds.segments.back().end, without.segments.back().end);
}

// ---------------------------------------------------------------------------
// Polygons
// ---------------------------------------------------------------------------

TEST_CASE("a polygon records a vertex wherever the turtle stands", "[turtle][polygon]") {
    // The turtle walks the outline; '.' pins a corner. Starting up the Y axis,
    // +(45) then two -(90) turns close a diamond in the XY plane.
    const turtle::Skeleton skeleton =
        turtle::buildSkeleton(word("{ . +(45) f(1) . -(90) f(1) . -(90) f(1) . }"));

    REQUIRE(skeleton.polygons.size() == 1);
    const std::vector<glm::vec3>& corners = skeleton.polygons.front().vertices;
    REQUIRE(corners.size() == 4);

    const float diagonal = std::sqrt(0.5f);
    requireVec3(corners[0], glm::vec3(0.0f, 0.0f, 0.0f));
    requireVec3(corners[1], glm::vec3(-diagonal, diagonal, 0.0f));
    requireVec3(corners[2], glm::vec3(0.0f, 2.0f * diagonal, 0.0f));
    requireVec3(corners[3], glm::vec3(diagonal, diagonal, 0.0f));
}

TEST_CASE("a leaf inherits the orientation of the twig that carries it",
          "[turtle][polygon]") {
    // Nothing special makes this happen: the leaf is traced by the same turtle,
    // so it starts in whatever frame the branch left behind.
    const turtle::Skeleton upright =
        turtle::buildSkeleton(word("{ . +(45) f(1) . -(90) f(1) . }"));
    const turtle::Skeleton rolled =
        turtle::buildSkeleton(word("\\(90) { . +(45) f(1) . -(90) f(1) . }"));

    REQUIRE(upright.polygons.size() == 1);
    REQUIRE(rolled.polygons.size() == 1);
    // The upright leaf lies in XY; rolling a quarter turn swings it into ZY.
    CHECK(std::fabs(upright.polygons.front().vertices[1].x) > 0.5f);
    CHECK(std::fabs(upright.polygons.front().vertices[1].z) < 1e-5f);
    CHECK(std::fabs(rolled.polygons.front().vertices[1].x) < 1e-5f);
    CHECK(std::fabs(rolled.polygons.front().vertices[1].z) > 0.5f);
}

TEST_CASE("polygons survive branch push and pop", "[turtle][polygon]") {
    // An outline is normally traced across branches, so the polygon stack has
    // to be independent of the [ ] state stack.
    const turtle::Skeleton skeleton =
        turtle::buildSkeleton(word("{ . [ +(45) f(1) . ] [ -(45) f(1) . ] }"));

    REQUIRE(skeleton.polygons.size() == 1);
    CHECK(skeleton.polygons.front().vertices.size() == 3);
}

TEST_CASE("an outline enclosing no area is discarded", "[turtle][polygon]") {
    CHECK(turtle::buildSkeleton(word("{ }")).polygons.empty());
    CHECK(turtle::buildSkeleton(word("{ . }")).polygons.empty());
    CHECK(turtle::buildSkeleton(word("{ . f(1) . }")).polygons.empty());
    CHECK(turtle::buildSkeleton(word("{ . f(1) . +(90) f(1) . }")).polygons.size() == 1);
}

TEST_CASE("a vertex outside any polygon is discarded", "[turtle][polygon]") {
    CHECK_NOTHROW(turtle::buildSkeleton(word(". F(1) .")));
    CHECK(turtle::buildSkeleton(word(". F(1) .")).polygons.empty());
}

TEST_CASE("unbalanced polygon braces are reported", "[turtle][polygon][errors]") {
    CHECK_THROWS_AS(turtle::buildSkeleton(word("{ . f(1) . f(1) .")), turtle::TurtleError);
    CHECK_THROWS_AS(turtle::buildSkeleton(word("F(1) }")), turtle::TurtleError);
}

TEST_CASE("polygons widen the bounds the voxelizer has to cover", "[turtle][polygon]") {
    // A leaf reaches well outside the twig it hangs from; a skeleton that only
    // measured its segments would have the voxelizer clip it.
    const turtle::Skeleton skeleton =
        turtle::buildSkeleton(word("F(1,0.02) { . +(90) f(2) . ^(90) f(2) . }"));

    REQUIRE(skeleton.polygons.size() == 1);
    CHECK(skeleton.boundsMin.x < -1.9f);
    CHECK(skeleton.boundsMax.z > 1.9f);
}

// ---------------------------------------------------------------------------
// Radii and bounds
// ---------------------------------------------------------------------------

TEST_CASE("segments taper into their thickest child", "[turtle]") {
    turtle::TurtleConfig config;
    config.tipTaper = 0.5f;

    const turtle::Skeleton skeleton =
        turtle::buildSkeleton(word("F(1,0.10)[+(30)F(1,0.04)]F(1,0.07)"), config);

    REQUIRE(skeleton.segments.size() == 3);
    // The trunk has two children (0.04 and 0.07); it narrows to the thicker one.
    CHECK_THAT(skeleton.segments[0].endRadius, WithinAbs(0.07, 1e-6));
    // Both children are leaves, so they taper to a tip.
    CHECK_THAT(skeleton.segments[1].endRadius, WithinAbs(0.02, 1e-6));
    CHECK_THAT(skeleton.segments[2].endRadius, WithinAbs(0.035, 1e-6));
}

TEST_CASE("a segment never widens towards its tip", "[turtle]") {
    // A child thicker than its parent would otherwise produce a cylinder that
    // flares outwards, which no real branch does.
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("F(1,0.05)F(1,0.20)"));

    REQUIRE(skeleton.segments.size() == 2);
    CHECK(skeleton.segments[0].endRadius <= skeleton.segments[0].startRadius);
}

TEST_CASE("! sets the width for subsequent segments", "[turtle]") {
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("!(0.3)F(1)"));

    REQUIRE(skeleton.segments.size() == 1);
    CHECK(skeleton.segments.front().startRadius == 0.3f);
}

TEST_CASE("bounds cover the cylinders, not just the axis", "[turtle]") {
    const turtle::Skeleton skeleton = turtle::buildSkeleton(word("F(2,0.25)"));

    requireVec3(skeleton.boundsMin, glm::vec3(-0.25f, -0.25f, -0.25f));
    // The tip tapers to half the start radius, so the top extends by 0.125.
    requireVec3(skeleton.boundsMax, glm::vec3(0.25f, 2.125f, 0.25f));
    CHECK_THAT(skeleton.maxRadius(), WithinAbs(0.25, 1e-6));
}

TEST_CASE("an empty word yields an empty skeleton with degenerate bounds", "[turtle]") {
    const turtle::Skeleton skeleton = turtle::buildSkeleton(lsystem::Word{});

    CHECK(skeleton.empty());
    CHECK(skeleton.maxRadius() == 0.0f);
    requireVec3(skeleton.size(), glm::vec3(0.0f));
}

// ---------------------------------------------------------------------------
// Presets end to end
// ---------------------------------------------------------------------------

TEST_CASE("every preset walks into a plausible plant", "[turtle][presets]") {
    for (const lsystem::GrammarSource& source : lsystem::presets::all()) {
        CAPTURE(source.name);

        const lsystem::LSystem system = lsystem::LSystem::compile(source);
        const turtle::Skeleton skeleton = turtle::buildSkeleton(system.expand(8, 1234));

        REQUIRE(skeleton.segments.size() > 20);
        CHECK(skeleton.boundsMax.y > 1.0f);

        // Genuinely three-dimensional. Measured on the segment axes rather than
        // on the bounds, because the bounds are inflated by the branch radius
        // and would hide a completely flat plant behind a non-zero extent.
        glm::vec3 axisMin(std::numeric_limits<float>::max());
        glm::vec3 axisMax(std::numeric_limits<float>::lowest());
        for (const turtle::Segment& segment : skeleton.segments) {
            axisMin = glm::min(axisMin, glm::min(segment.start, segment.end));
            axisMax = glm::max(axisMax, glm::max(segment.start, segment.end));
        }
        CHECK(axisMax.x - axisMin.x > 0.2f);
        CHECK(axisMax.z - axisMin.z > 0.2f);

        for (const turtle::Segment& segment : skeleton.segments) {
            CHECK(segment.length() > 0.0f);
            CHECK(segment.startRadius > 0.0f);
            CHECK(segment.endRadius > 0.0f);
            CHECK(std::isfinite(segment.start.x));
            CHECK(std::isfinite(segment.end.y));
        }
    }
}

TEST_CASE("the signal preset is broad at the base and tapers to a point",
          "[turtle][presets][context]") {
    // The age gradient no rule states: a bud dropped early by the climbing
    // signal has had many more steps to develop than one dropped near the top,
    // so the lower half of the plant reaches further from the stem.
    const lsystem::LSystem system = lsystem::LSystem::compile(lsystem::presets::signalStem());

    const int iterations = GENERATE(12, 16, 20, 24);
    CAPTURE(iterations);
    const turtle::Skeleton skeleton = turtle::buildSkeleton(system.expand(iterations));

    REQUIRE(skeleton.segments.size() > 50);
    const float base = skeleton.boundsMin.y;
    const float span = skeleton.boundsMax.y - base;

    // Compared by quartile rather than by half: the middle of the stem carries
    // buds of every intermediate age, which blurs the contrast.
    const auto reachWithin = [&](float lowFraction, float highFraction) {
        float furthest = 0.0f;
        for (const turtle::Segment& segment : skeleton.segments) {
            const float height = (0.5f * (segment.start.y + segment.end.y) - base) / span;
            if (height < lowFraction || height > highFraction) {
                continue;
            }
            // Distance from the stem, which grows straight up the Y axis.
            furthest = std::max(furthest, std::hypot(segment.end.x, segment.end.z));
        }
        return furthest;
    };

    const float bottom = reachWithin(0.0f, 0.25f);
    const float top = reachWithin(0.75f, 1.0f);
    CAPTURE(bottom, top);
    // Measured around 1.5x; the bound is set below that and checked across a
    // range of ages, since the gradient is strongest while the youngest buds
    // are still filling in.
    CHECK(bottom > top * 1.3f);
}

TEST_CASE("the skeleton is a function of the word alone", "[turtle][presets]") {
    const lsystem::LSystem system = lsystem::LSystem::compile(lsystem::presets::bush());
    const lsystem::Word expanded = system.expand(6, 99);

    const turtle::Skeleton first = turtle::buildSkeleton(expanded);
    const turtle::Skeleton second = turtle::buildSkeleton(expanded);

    REQUIRE(first.segments.size() == second.segments.size());
    for (std::size_t i = 0; i < first.segments.size(); ++i) {
        requireVec3(first.segments[i].end, second.segments[i].end);
    }
}
