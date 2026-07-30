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
    glm::vec3 crownCentre{0.0f, 1.9f, 0.0f};
    glm::vec3 crownRadii{1.1f, 1.0f, 1.1f};
    int attractorCount = 650;

    /// How far a point can reach to influence a node. Too small and growth
    /// stalls; too large and every point pulls on everything, which averages
    /// into a single fat trunk.
    float influenceRadius = 0.7f;
    /// How close a node must get before the point is considered reached and
    /// removed. Large relative to the step and each tip clears a wide sphere per
    /// advance, which is what turns branches into short stubs.
    float killDistance = 0.14f;
    /// How far a node advances per iteration.
    float stepLength = 0.06f;
    int maxIterations = 600;

    /// How much of a branch's existing direction carries into its next step.
    /// At zero a tip follows the raw average of whatever is pulling it and
    /// wanders visibly; the momentum is what makes a limb read as a limb.
    float straightness = 0.55f;

    /// Edge of the diamond leaf placed at every branch tip. Zero grows a bare
    /// winter tree.
    ///
    /// Worth having rather than colouring the twigs green: a leafless structure
    /// painted green claims foliage where there is only wood, which is most of
    /// why the first version of this looked wrong.
    float leafSize = 0.14f;

    glm::vec3 origin{0.0f};
    /// The trunk grows straight towards the crown until something is in reach,
    /// otherwise a crown placed above the root would never be found.
    int maxTrunkSteps = 200;

    /// Radius at the base of the trunk. Radii are solved by the taper rule and
    /// then scaled so the root lands exactly here, which is the only way to keep
    /// trunk thickness controllable: the raw rule's answer depends on how many
    /// twigs happened to grow.
    float trunkRadius = 0.07f;
    /// Floor for the thinnest twig.
    float tipRadius = 0.014f;
    /// Pipe-model exponent. A node's radius goes as the total branch length
    /// above it, raised to 1/n, so `r^n = ownLength + sum of child r^n`.
    ///
    /// Length-driven rather than Murray's law on tip counts, which is *exactly*
    /// constant along an unbranched chain. Accumulating length makes it strictly
    /// decreasing instead, though only slightly over a short bole: nearly all the
    /// length lives in the crown. Raising n tapers more *gently*, since
    /// r/rRoot = (acc/accRoot)^(1/n) and a ratio below one raised to a smaller
    /// power sits closer to one.
    float taperExponent = 2.2f;
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
