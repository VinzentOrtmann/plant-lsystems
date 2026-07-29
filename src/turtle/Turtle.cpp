#include "turtle/Turtle.h"

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/mat3x3.hpp>
#include <glm/trigonometric.hpp>

#include <algorithm>
#include <limits>

namespace plant::turtle {

float Segment::length() const {
    return glm::length(end - start);
}

glm::vec3 Segment::direction() const {
    const glm::vec3 delta = end - start;
    const float len = glm::length(delta);
    return len > 0.0f ? delta / len : glm::vec3(0.0f, 1.0f, 0.0f);
}

float Skeleton::maxRadius() const {
    float radius = 0.0f;
    for (const Segment& segment : segments) {
        radius = std::max({radius, segment.startRadius, segment.endRadius});
    }
    return radius;
}

int Skeleton::maxDepth() const {
    int depth = 0;
    for (const Segment& segment : segments) {
        depth = std::max(depth, segment.depth);
    }
    return depth;
}

namespace {

/// Turtle-local frame axes. The identity orientation maps these to
/// H = +Y, L = -X, U = +Z, which satisfies H x L = U.
constexpr glm::vec3 kLocalHeading(0.0f, 1.0f, 0.0f);
constexpr glm::vec3 kLocalLeft(-1.0f, 0.0f, 0.0f);
constexpr glm::vec3 kLocalUp(0.0f, 0.0f, 1.0f);

struct State {
    glm::vec3 position{0.0f};
    glm::quat orientation{1.0f, 0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    int parent = -1;
    int depth = 0;
};

float paramOr(const lsystem::Module& module, std::size_t index, float fallback) {
    return index < module.params.size() ? module.params[index] : fallback;
}

/// Fills in end radii: a segment tapers to whatever its thickest child starts
/// at, so a trunk narrows smoothly into its branches instead of stepping down.
/// Segments with no children taper to a point-ish tip.
void applyTaper(std::vector<Segment>& segments, float tipTaper) {
    std::vector<float> widestChild(segments.size(), 0.0f);
    for (const Segment& segment : segments) {
        if (segment.parent >= 0) {
            float& widest = widestChild[static_cast<std::size_t>(segment.parent)];
            widest = std::max(widest, segment.startRadius);
        }
    }
    for (std::size_t i = 0; i < segments.size(); ++i) {
        segments[i].endRadius = widestChild[i] > 0.0f ? std::min(widestChild[i], segments[i].startRadius)
                                                      : segments[i].startRadius * tipTaper;
    }
}

void computeBounds(Skeleton& skeleton) {
    if (skeleton.empty()) {
        skeleton.boundsMin = glm::vec3(0.0f);
        skeleton.boundsMax = glm::vec3(0.0f);
        return;
    }
    glm::vec3 lo(std::numeric_limits<float>::max());
    glm::vec3 hi(std::numeric_limits<float>::lowest());
    for (const Segment& segment : skeleton.segments) {
        // The cylinder bulges past its axis by its radius at each end.
        lo = glm::min(lo, segment.start - glm::vec3(segment.startRadius));
        hi = glm::max(hi, segment.start + glm::vec3(segment.startRadius));
        lo = glm::min(lo, segment.end - glm::vec3(segment.endRadius));
        hi = glm::max(hi, segment.end + glm::vec3(segment.endRadius));
    }
    for (const Polygon& polygon : skeleton.polygons) {
        for (const glm::vec3& vertex : polygon.vertices) {
            lo = glm::min(lo, vertex);
            hi = glm::max(hi, vertex);
        }
    }
    skeleton.boundsMin = lo;
    skeleton.boundsMax = hi;
}

}  // namespace

Skeleton buildSkeleton(const lsystem::Word& word, const TurtleConfig& config) {
    Skeleton skeleton;
    std::vector<State> stack;
    std::vector<Polygon> polygons;  // currently open, innermost last

    State state;
    state.position = config.origin;
    state.orientation = glm::normalize(config.orientation);
    state.radius = config.defaultRadius;

    const auto rotate = [&](const glm::vec3& localAxis, float degrees) {
        const float radians = glm::radians(degrees * config.angleScale);
        state.orientation = glm::normalize(state.orientation * glm::angleAxis(radians, localAxis));
    };

    const auto heading = [&] { return glm::normalize(state.orientation * kLocalHeading); };

    const bool tropismActive =
        config.tropismStrength != 0.0f && glm::dot(config.tropism, config.tropism) > 0.0f;
    const glm::vec3 tropismDirection =
        tropismActive ? glm::normalize(config.tropism) : glm::vec3(0.0f);

    /// Bends the heading towards the tropism vector. The cross product is in
    /// world space, so this rotation pre-multiplies the orientation rather than
    /// composing into the turtle's local frame like the rotation symbols do.
    const auto applyTropism = [&] {
        if (!tropismActive) {
            return;
        }
        const glm::vec3 axis = glm::cross(heading(), tropismDirection);
        const float sine = glm::length(axis);
        if (sine <= 1e-6f) {
            return;  // already aligned with the tropism: nothing to bend
        }
        const float angle = config.tropismStrength * sine;
        state.orientation =
            glm::normalize(glm::angleAxis(angle, axis / sine) * state.orientation);
    };

    for (const lsystem::Module& module : word) {
        switch (module.symbol) {
            case 'F': {
                const float length = paramOr(module, 0, config.defaultLength);
                const float radius = std::max(0.0f, paramOr(module, 1, state.radius));
                state.radius = radius;
                if (length <= 0.0f) {
                    break;  // nothing to draw, and a zero-length cylinder has no axis
                }
                Segment segment;
                segment.start = state.position;
                segment.end = state.position + heading() * length;
                // state.radius stays in grammar units; the scale is applied on
                // the way out so that ! and F(l,w) keep composing as written.
                segment.startRadius = radius * config.radiusScale;
                segment.endRadius = segment.startRadius;  // replaced by applyTaper below
                segment.parent = state.parent;
                segment.depth = state.depth;
                skeleton.segments.push_back(segment);

                state.parent = static_cast<int>(skeleton.segments.size()) - 1;
                state.position = segment.end;
                // Applied after the segment, so bending accumulates along a
                // chain: the segment just drawn is straight, the next one leans.
                applyTropism();
                break;
            }

            case 'f':
                state.position += heading() * paramOr(module, 0, config.defaultLength);
                break;

            case '+':  rotate(kLocalUp,       paramOr(module, 0, config.defaultAngle)); break;
            case '-':  rotate(kLocalUp,      -paramOr(module, 0, config.defaultAngle)); break;
            case '&':  rotate(kLocalLeft,     paramOr(module, 0, config.defaultAngle)); break;
            case '^':  rotate(kLocalLeft,    -paramOr(module, 0, config.defaultAngle)); break;
            case '\\': rotate(kLocalHeading,  paramOr(module, 0, config.defaultAngle)); break;
            case '/':  rotate(kLocalHeading, -paramOr(module, 0, config.defaultAngle)); break;

            case '$': {
                // ABOP's "roll upright". Branches that have been pitched and
                // rolled arrive carrying an arbitrary twist; re-levelling them
                // is what stops sub-branches from spiralling off sideways.
                const glm::vec3 forward = heading();
                const glm::vec3 sideways = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), forward);
                const float extent = glm::length(sideways);
                if (extent > 1e-5f) {  // undefined when the heading is already vertical
                    const glm::vec3 left = sideways / extent;
                    const glm::vec3 up = glm::cross(forward, left);
                    // Columns are the images of local +X, +Y, +Z, and the local
                    // frame is H = +Y, L = -X, U = +Z.
                    state.orientation = glm::normalize(glm::quat_cast(glm::mat3(-left, forward, up)));
                }
                break;
            }

            case '!':
                state.radius = std::max(0.0f, paramOr(module, 0, state.radius * 0.7f));
                break;

            case '{':
                // Independent of the [ ] stack: a polygon outline is normally
                // traced across branches, so it must survive push and pop.
                polygons.push_back(Polygon{{}, state.depth});
                break;

            case '.':
                if (!polygons.empty()) {
                    polygons.back().vertices.push_back(state.position);
                }
                break;  // a vertex outside any polygon is simply discarded

            case '}': {
                if (polygons.empty()) {
                    throw TurtleError("unbalanced '}': the word closes a polygon that was "
                                      "never opened");
                }
                Polygon finished = std::move(polygons.back());
                polygons.pop_back();
                if (finished.vertices.size() >= 3) {
                    skeleton.polygons.push_back(std::move(finished));
                }
                break;  // fewer than three vertices encloses no area
            }

            case '[':
                stack.push_back(state);
                ++state.depth;
                break;

            case ']':
                if (stack.empty()) {
                    throw TurtleError("unbalanced ']': the word closes a branch that was never opened");
                }
                state = stack.back();
                stack.pop_back();
                break;

            default:
                break;  // buds and any other symbol carry no geometry
        }
    }

    if (!stack.empty()) {
        throw TurtleError("unbalanced '[': " + std::to_string(stack.size()) +
                          " branch(es) were opened and never closed");
    }
    if (!polygons.empty()) {
        throw TurtleError("unbalanced '{': " + std::to_string(polygons.size()) +
                          " polygon(s) were opened and never closed");
    }

    applyTaper(skeleton.segments, config.tipTaper);
    computeBounds(skeleton);
    return skeleton;
}

}  // namespace plant::turtle
