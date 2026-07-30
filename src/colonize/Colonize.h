#pragma once

#include "turtle/Turtle.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

/// Space colonization, after Runions et al., "Modeling Trees with a Space
/// Colonization Algorithm" (2007).
///
/// A completely different way to grow a tree from the L-system next door, and
/// the reason it is worth having both. An L-system is *generative*: rules say
/// what a bud becomes, and the shape is whatever falls out. Space colonization
/// is *responsive*: a cloud of attraction points describes the volume the crown
/// should occupy, and branches grow towards whichever points are near them,
/// consuming points as they arrive. Give it a different cloud and it fills a
/// different shape without a single rule changing.
///
/// It produces a turtle::Skeleton directly, so everything downstream --
/// voxelizer, .vox exporter, viewer, metrics, seed sheets -- consumes it
/// unchanged. That is the whole point of the seam.
///
/// (The Skeleton type lives in Turtle.h for historical reasons; it is really
/// the pipeline's shared intermediate representation, not a turtle detail.)
namespace plant::colonize {

struct ColonizeConfig {
    /// Attraction points are sampled uniformly inside this ellipsoid: the
    /// volume the crown will grow to fill.
    glm::vec3 crownCentre{0.0f, 2.2f, 0.0f};
    glm::vec3 crownRadii{1.2f, 1.4f, 1.2f};
    int attractorCount = 900;

    /// How far a point can reach to influence a node. Too small and growth
    /// stalls; too large and every point pulls on everything, which averages
    /// into a single fat trunk.
    float influenceRadius = 0.75f;
    /// How close a node must get before the point is considered reached and
    /// removed. This sets the density of the twigs.
    float killDistance = 0.16f;
    /// How far a node advances per iteration.
    float stepLength = 0.09f;
    int maxIterations = 400;

    glm::vec3 origin{0.0f};
    /// The trunk grows straight towards the crown until something is in reach,
    /// otherwise a crown placed above the root would never be found.
    int maxTrunkSteps = 200;

    /// Radius of a twig with no children.
    float tipRadius = 0.012f;
    /// Murray's law exponent: parentRadius^n = sum of childRadius^n. 2 conserves
    /// cross-sectional area through a fork; real trees measure nearer 2.5.
    float radiusExponent = 2.4f;
};

struct ColonizeStats {
    int iterations = 0;
    std::size_t nodes = 0;
    /// Attraction points consumed; the rest were never reached.
    std::size_t attractorsReached = 0;
    std::size_t attractorsTotal = 0;
};

/// Grows a tree and returns its skeleton. Fully determined by (config, seed).
[[nodiscard]] turtle::Skeleton grow(const ColonizeConfig& config, std::uint32_t seed,
                                    ColonizeStats* stats = nullptr);

/// The attraction cloud on its own, for visualising what the tree is reaching
/// for. Uses the same seed and sampling as grow().
[[nodiscard]] std::vector<glm::vec3> sampleAttractors(const ColonizeConfig& config,
                                                      std::uint32_t seed);

}  // namespace plant::colonize
