#include "colonize/Colonize.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

namespace plant::colonize {
namespace {

/// Uniform in [0,1). Hand-rolled for the same reason the L-system's draw is:
/// std::uniform_real_distribution's output is not specified across standard
/// library implementations, and a seed has to grow the same tree everywhere.
float canonicalRandom(std::mt19937& rng) {
    return static_cast<float>(static_cast<double>(rng()) * (1.0 / 4294967296.0));
}

struct Node {
    glm::vec3 position{0.0f};
    int parent = -1;
    int depth = 0;
    /// Summed unit vectors towards the points influencing this node, this
    /// iteration only.
    glm::vec3 pull{0.0f};
    int pullCount = 0;
    int childCount = 0;
};

/// Uniform grid over node positions.
///
/// The naive loop is "for every attractor, scan every node", which is O(A*N)
/// per iteration and the dominant cost by far -- a few hundred attractors
/// against a few thousand nodes, four hundred times over. Bucketing nodes by
/// influence-radius-sized cells turns the scan into 27 cell lookups.
class NodeGrid {
public:
    explicit NodeGrid(float cellSize) : cellSize_(std::max(cellSize, 1e-4f)) {}

    void insert(int index, const glm::vec3& position) {
        cells_[key(cellOf(position))].push_back(index);
    }

    /// Calls `visit(index)` for every node in the cells a sphere of `radius`
    /// touches. `radius` must not exceed the cell size, which is how the search
    /// stays a fixed 27 cells.
    template <typename Visit>
    void forEachNear(const glm::vec3& position, Visit&& visit) const {
        const glm::ivec3 centre = cellOf(position);
        for (int z = -1; z <= 1; ++z) {
            for (int y = -1; y <= 1; ++y) {
                for (int x = -1; x <= 1; ++x) {
                    const auto found = cells_.find(key(centre + glm::ivec3(x, y, z)));
                    if (found == cells_.end()) {
                        continue;
                    }
                    for (const int index : found->second) {
                        visit(index);
                    }
                }
            }
        }
    }

private:
    [[nodiscard]] glm::ivec3 cellOf(const glm::vec3& position) const {
        return glm::ivec3(glm::floor(position / cellSize_));
    }
    static std::int64_t key(const glm::ivec3& cell) {
        // Offset keeps the components non-negative before packing.
        constexpr std::int64_t bias = 1 << 20;
        return ((static_cast<std::int64_t>(cell.x) + bias) << 42) |
               ((static_cast<std::int64_t>(cell.y) + bias) << 21) |
               (static_cast<std::int64_t>(cell.z) + bias);
    }

    float cellSize_;
    std::unordered_map<std::int64_t, std::vector<int>> cells_;
};

/// Pipe model, applied from the twigs inwards: a branch is thick in proportion
/// to the total length of wood it carries. Nodes are appended after their
/// parent, so one reverse pass suffices.
///
/// Accumulating *length* rather than tip counts is what makes a trunk taper.
/// Murray's law on tip counts is right at a fork but constant along an
/// unbranched chain, which renders a trunk as a uniform tube. Here
/// `r^n = ownLength + sum of child r^n`, so the extra own-length term shrinks
/// the radius at every step even where nothing branches.
///
/// The result is then scaled so the root lands on `trunkRadius`: the raw rule's
/// absolute answer depends on how many twigs happened to grow, which is not
/// something a user should have to compensate for.
std::vector<float> solveRadii(const std::vector<Node>& nodes, float trunkRadius, float tipRadius,
                              float exponent) {
    const auto power = static_cast<double>(std::max(0.5f, exponent));
    std::vector<double> carried(nodes.size(), 0.0);

    for (std::size_t i = nodes.size(); i-- > 1;) {
        const auto parent = static_cast<std::size_t>(nodes[i].parent);
        carried[i] += glm::distance(nodes[i].position, nodes[parent].position);
        carried[parent] += carried[i];
    }

    std::vector<float> radius(nodes.size(), tipRadius);
    const double rootCarried = std::max(carried[0], 1e-9);
    const double scale = static_cast<double>(trunkRadius) / std::pow(rootCarried, 1.0 / power);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const double raw = std::pow(std::max(carried[i], 0.0), 1.0 / power) * scale;
        radius[i] = std::max(tipRadius, static_cast<float>(raw));
    }
    return radius;
}

/// A diamond leaf at every childless node, lying along the branch it caps and
/// rolled by a random amount so a canopy is not a field of parallel plates.
void growLeaves(turtle::Skeleton& skeleton, const std::vector<Node>& nodes, float size,
                std::mt19937& rng) {
    if (size <= 0.0f) {
        return;
    }
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        if (nodes[i].childCount != 0) {
            continue;
        }
        const auto parent = static_cast<std::size_t>(nodes[i].parent);
        const glm::vec3 along = nodes[i].position - nodes[parent].position;
        const float travelled = glm::length(along);
        if (travelled < 1e-6f) {
            continue;
        }
        const glm::vec3 forward = along / travelled;

        // Any vector off the branch axis will do for the leaf's width; picking
        // the world axis the branch leans on least keeps the cross product well
        // conditioned however the twig happens to point.
        const glm::vec3 fallback = std::fabs(forward.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f)
                                                              : glm::vec3(1.0f, 0.0f, 0.0f);
        glm::vec3 across = glm::normalize(glm::cross(forward, fallback));
        const glm::vec3 other = glm::cross(forward, across);
        const float roll = canonicalRandom(rng) * 6.28318531f;
        across = glm::normalize(across * std::cos(roll) + other * std::sin(roll));

        const glm::vec3 base = nodes[i].position;
        const float half = size * 0.5f;
        skeleton.polygons.push_back(turtle::Polygon{
            {base, base + forward * half + across * half, base + forward * size,
             base + forward * half - across * half},
            nodes[i].depth});
    }
}

}  // namespace

std::vector<glm::vec3> sampleAttractors(const ColonizeConfig& config, std::uint32_t seed) {
    std::vector<glm::vec3> points;
    const int wanted = std::max(0, config.attractorCount);
    points.reserve(static_cast<std::size_t>(wanted));

    std::mt19937 rng(seed);
    // Rejection sampling in the unit sphere, then scaled: sampling each axis
    // independently would bias points towards the corners of the box.
    while (static_cast<int>(points.size()) < wanted) {
        const glm::vec3 candidate(canonicalRandom(rng) * 2.0f - 1.0f,
                                  canonicalRandom(rng) * 2.0f - 1.0f,
                                  canonicalRandom(rng) * 2.0f - 1.0f);
        if (glm::dot(candidate, candidate) > 1.0f) {
            continue;
        }
        points.push_back(config.crownCentre + candidate * config.crownRadii);
    }
    return points;
}

turtle::Skeleton grow(const ColonizeConfig& config, std::uint32_t seed, ColonizeStats* stats) {
    std::vector<glm::vec3> attractors = sampleAttractors(config, seed);
    std::vector<bool> alive(attractors.size(), true);
    std::size_t remaining = attractors.size();

    std::vector<Node> nodes;
    nodes.push_back(Node{config.origin, -1, 0, glm::vec3(0.0f), 0, 0});

    const float influence = std::max(config.influenceRadius, 1e-4f);
    const float step = std::max(config.stepLength, 1e-4f);
    const float kill = std::max(config.killDistance, 0.0f);

    NodeGrid grid(influence);
    grid.insert(0, nodes[0].position);

    const auto anyAttractorInReach = [&](const glm::vec3& position) {
        for (std::size_t i = 0; i < attractors.size(); ++i) {
            if (alive[i] && glm::distance(attractors[i], position) <= influence) {
                return true;
            }
        }
        return false;
    };

    // Trunk: grow straight at the crown until it comes into reach. Without this
    // a crown placed above the root is simply never found, and the algorithm
    // produces a single node.
    for (int trunkStep = 0; trunkStep < config.maxTrunkSteps; ++trunkStep) {
        if (remaining == 0 || anyAttractorInReach(nodes.back().position)) {
            break;
        }
        const glm::vec3 toCrown = config.crownCentre - nodes.back().position;
        const float distance = glm::length(toCrown);
        if (distance < 1e-5f) {
            break;
        }
        const int parent = static_cast<int>(nodes.size()) - 1;
        nodes[static_cast<std::size_t>(parent)].childCount++;
        nodes.push_back(Node{nodes.back().position + (toCrown / distance) * step, parent, 0,
                             glm::vec3(0.0f), 0, 0});
        grid.insert(static_cast<int>(nodes.size()) - 1, nodes.back().position);
    }

    int iterations = 0;
    for (; iterations < config.maxIterations && remaining > 0; ++iterations) {
        for (Node& node : nodes) {
            node.pull = glm::vec3(0.0f);
            node.pullCount = 0;
        }

        // Each live point pulls on the single nearest node within reach.
        for (std::size_t i = 0; i < attractors.size(); ++i) {
            if (!alive[i]) {
                continue;
            }
            int nearest = -1;
            float nearestDistance = influence;
            grid.forEachNear(attractors[i], [&](int index) {
                const float distance = glm::distance(attractors[i], nodes[static_cast<std::size_t>(index)].position);
                if (distance <= nearestDistance) {
                    nearestDistance = distance;
                    nearest = index;
                }
            });
            if (nearest < 0) {
                continue;
            }
            Node& node = nodes[static_cast<std::size_t>(nearest)];
            const glm::vec3 towards = attractors[i] - node.position;
            const float length = glm::length(towards);
            if (length > 1e-6f) {
                node.pull += towards / length;
                ++node.pullCount;
            }
        }

        // Snapshot: nodes added below must not themselves grow this iteration.
        const std::size_t before = nodes.size();
        for (std::size_t i = 0; i < before; ++i) {
            if (nodes[i].pullCount == 0) {
                continue;
            }
            const float length = glm::length(nodes[i].pull);
            if (length < 1e-6f) {
                continue;  // pulls cancelled out exactly
            }
            // Blend in the direction this branch is already travelling. Without
            // it a tip chases the instantaneous average of its attractors and
            // the limb visibly zigzags.
            glm::vec3 direction = nodes[i].pull / length;
            if (config.straightness > 0.0f) {
                const glm::vec3 travelling =
                    nodes[i].parent >= 0
                        ? nodes[i].position - nodes[static_cast<std::size_t>(nodes[i].parent)].position
                        : glm::vec3(0.0f, 1.0f, 0.0f);
                const float travelled = glm::length(travelling);
                if (travelled > 1e-6f) {
                    const glm::vec3 blended =
                        direction + (travelling / travelled) * config.straightness;
                    const float blendedLength = glm::length(blended);
                    if (blendedLength > 1e-6f) {
                        direction = blended / blendedLength;
                    }
                }
            }
            // The first child continues the branch; later ones start new ones,
            // which is what the depth means downstream when colouring.
            const int depth = nodes[i].childCount == 0 ? nodes[i].depth : nodes[i].depth + 1;
            nodes[i].childCount++;
            nodes.push_back(Node{nodes[i].position + direction * step, static_cast<int>(i), depth,
                                 glm::vec3(0.0f), 0, 0});
            grid.insert(static_cast<int>(nodes.size()) - 1, nodes.back().position);
        }

        if (nodes.size() == before) {
            break;  // nothing in reach of anything: growth has stalled
        }

        for (std::size_t i = 0; i < attractors.size(); ++i) {
            if (!alive[i]) {
                continue;
            }
            for (std::size_t n = before; n < nodes.size(); ++n) {
                if (glm::distance(attractors[i], nodes[n].position) <= kill) {
                    alive[i] = false;
                    --remaining;
                    break;
                }
            }
        }
    }

    const std::vector<float> radii =
        solveRadii(nodes, config.trunkRadius, config.tipRadius, config.taperExponent);

    turtle::Skeleton skeleton;
    skeleton.segments.reserve(nodes.size() > 0 ? nodes.size() - 1 : 0);
    for (std::size_t i = 1; i < nodes.size(); ++i) {
        const auto parent = static_cast<std::size_t>(nodes[i].parent);
        turtle::Segment segment;
        segment.start = nodes[parent].position;
        segment.end = nodes[i].position;
        segment.startRadius = radii[parent];
        segment.endRadius = radii[i];
        // Node 0 has no incoming segment, so node k maps to segment k-1 and the
        // root's children become roots here too.
        segment.parent = nodes[i].parent == 0 ? -1 : nodes[i].parent - 1;
        segment.depth = nodes[i].depth;
        skeleton.segments.push_back(segment);
    }

    // Seeded separately from the attraction cloud so that changing the leaf size
    // does not reshuffle the tree it hangs on.
    std::mt19937 leafRng(seed ^ 0x9E3779B9u);
    growLeaves(skeleton, nodes, config.leafSize, leafRng);
    turtle::recomputeBounds(skeleton);

    if (stats != nullptr) {
        stats->iterations = iterations;
        stats->nodes = nodes.size();
        stats->attractorsTotal = attractors.size();
        stats->attractorsReached = attractors.size() - remaining;
    }
    return skeleton;
}

}  // namespace plant::colonize
