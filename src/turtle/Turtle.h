#pragma once

#include "lsystem/LSystem.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <stdexcept>
#include <vector>

/// Turtle geometry: walks an expanded L-system word and builds a 3D branch
/// skeleton.
///
/// The turtle carries a position and an orthonormal frame (heading H, left L,
/// up U, with H x L = U). Symbols either move it, rotate it, or save/restore
/// its state:
///
///     F(len,width)  draw a segment and move to its end
///     f(len)        move without drawing
///     +(a) -(a)     yaw left/right   (rotate about U)
///     &(a) ^(a)     pitch down/up    (rotate about L)
///     \(a) /(a)     roll left/right  (rotate about H)
///     $             roll upright: spin about H until U points as close to
///                   world up as it can, without moving the heading
///     !(width)      set the current width
///     [ ]           push/pop the turtle state
///     { }           begin/end a filled polygon
///     .             record the current position as a polygon vertex
///
/// Parameters are optional everywhere: a bare `+` uses TurtleConfig::defaultAngle,
/// and `F` uses defaultLength/defaultRadius. Any symbol without a meaning here
/// is skipped, which is what lets non-drawing buds (`A`, `B`) survive in a word
/// that was not expanded to completion.
namespace plant::turtle {

/// One branch segment: a tapered cylinder from `start` to `end`.
struct Segment {
    glm::vec3 start{0.0f};
    glm::vec3 end{0.0f};
    float startRadius = 0.0f;
    float endRadius = 0.0f;
    /// Index of the segment this one grows out of, or -1 for a root.
    int parent = -1;
    /// Bracket nesting depth at the time of drawing. 0 is the trunk.
    int depth = 0;

    [[nodiscard]] float length() const;
    /// Unit vector from start to end. Returns +Y for a degenerate segment.
    [[nodiscard]] glm::vec3 direction() const;
};

/// A flat filled surface -- a leaf or a petal -- recorded between `{` and `}`.
/// Vertices are in the order the turtle visited them and the outline is
/// implicitly closed. Nothing forces them to be coplanar or convex; the
/// voxelizer fans them into triangles, which is exact for the convex shapes
/// grammars actually produce.
struct Polygon {
    std::vector<glm::vec3> vertices;
    /// Bracket nesting depth where the polygon was opened.
    int depth = 0;
};

struct Skeleton {
    std::vector<Segment> segments;
    std::vector<Polygon> polygons;
    /// Axis-aligned bounds including each segment's radius and every polygon
    /// vertex, so this is the box the voxelizer has to cover. Both corners are
    /// zero for an empty skeleton.
    glm::vec3 boundsMin{0.0f};
    glm::vec3 boundsMax{0.0f};

    [[nodiscard]] bool empty() const { return segments.empty() && polygons.empty(); }
    [[nodiscard]] glm::vec3 size() const { return boundsMax - boundsMin; }
    [[nodiscard]] float maxRadius() const;
    /// Largest bracket nesting depth reached, useful for colouring.
    [[nodiscard]] int maxDepth() const;
};

struct TurtleConfig {
    /// Used by rotation symbols that carry no parameter of their own.
    float defaultAngle = 25.0f;
    float defaultLength = 1.0f;
    float defaultRadius = 0.05f;
    /// A segment with no children tapers to this fraction of its start radius,
    /// so branches end in points rather than flat stumps.
    float tipTaper = 0.5f;
    /// Multiplies every rotation. Lets the viewer sweep the branch angle without
    /// recompiling the grammar, since angles baked into constants are otherwise
    /// fixed at compile time.
    float angleScale = 1.0f;
    /// Multiplies every segment radius, for the same reason.
    float radiusScale = 1.0f;

    /// Tropism, after the model in The Algorithmic Beauty of Plants: after each
    /// drawn segment the turtle is rotated by `tropismStrength * |H x T|`
    /// radians about `H x T`, bending the heading towards `tropism`. The
    /// magnitude falls to zero as the heading lines up with the tropism, so a
    /// vertical trunk under gravity stays straight while a horizontal limb sags.
    ///
    /// Defaults to straight down. A negative strength bends away instead, which
    /// is how you get phototropism out of the same knob.
    glm::vec3 tropism{0.0f, -1.0f, 0.0f};
    float tropismStrength = 0.0f;

    glm::vec3 origin{0.0f};
    /// Identity points the turtle along +Y with L = -X and U = +Z.
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
};

/// Thrown for words the turtle cannot walk, i.e. unbalanced brackets.
struct TurtleError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Interprets `word` into a branch skeleton. Segments appear in drawing order,
/// so a segment's parent always has a lower index than the segment itself.
Skeleton buildSkeleton(const lsystem::Word& word, const TurtleConfig& config = {});

/// Recomputes `boundsMin`/`boundsMax` from the segments and polygons present.
/// Exposed because the turtle is not the only thing that builds a Skeleton.
void recomputeBounds(Skeleton& skeleton);

}  // namespace plant::turtle
