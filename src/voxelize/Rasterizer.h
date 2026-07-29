#pragma once

#include "turtle/Turtle.h"
#include "voxelize/VoxelGrid.h"

#include <stdexcept>

/// Skeleton -> voxels.
///
/// Each branch segment is swept as a sphere whose radius interpolates from the
/// segment's start radius to its end radius -- a "round cone". Voxels whose
/// centre falls inside that solid are filled. Consecutive segments share an
/// endpoint and a radius there, so their swept spheres coincide and joints come
/// out closed without any special handling.
namespace plant::voxelize {

struct RasterizerConfig {
    /// Edge length of one voxel in world units. Use voxelSizeForResolution() to
    /// derive it from a target grid size.
    float voxelSize = 0.05f;

    /// Floor on the radius, in voxels. Half a voxel diagonal (sqrt(3)/2) is the
    /// smallest radius that guarantees every point of a segment's axis has a
    /// voxel centre within reach, which is what keeps the thinnest twigs from
    /// rasterizing to a dotted line.
    float minRadiusVoxels = 0.87f;

    /// Branch depth is mapped onto colour indices firstColor .. firstColor +
    /// colorCount - 1, giving a bark-to-leaf ramp. The .vox exporter writes a
    /// palette to match.
    ColorIndex firstColor = 1;
    int colorCount = 8;

    /// Polygons are filled as slabs this many voxels thick, in their own colour
    /// slot just past the branch ramp.
    float polygonThicknessVoxels = 1.2f;
    ColorIndex polygonColor = 9;

    /// Refuses to build a grid larger than this on any axis. MagicaVoxel's own
    /// limit is 256; the default leaves room to preview at higher resolution
    /// than you export.
    int maxDimension = 512;
};

struct VoxelizeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Voxel size that makes the skeleton's longest axis about `longestAxisVoxels`
/// voxels across. Returns 1.0 for an empty or degenerate skeleton.
[[nodiscard]] float voxelSizeForResolution(const turtle::Skeleton& skeleton,
                                           int longestAxisVoxels);

/// Rasterizes every segment into a sparse grid sized to the skeleton's bounds
/// plus one voxel of padding. Throws VoxelizeError if the voxel size would
/// produce a grid past config.maxDimension.
[[nodiscard]] VoxelGrid voxelize(const turtle::Skeleton& skeleton,
                                 const RasterizerConfig& config = {});

/// True if `point` lies inside the round cone swept from sphere (a, radiusA) to
/// sphere (b, radiusB). Exposed for testing -- this predicate is the whole of
/// the rasterizer's geometry for branches.
[[nodiscard]] bool insideRoundCone(const glm::vec3& point, const glm::vec3& a, const glm::vec3& b,
                                   float radiusA, float radiusB);

/// Point on triangle abc nearest to `point`, by Voronoi region. Exposed for
/// testing -- it is what gives polygons an exact slab of uniform thickness
/// rather than one that thins out towards the edges.
[[nodiscard]] glm::vec3 closestPointOnTriangle(const glm::vec3& point, const glm::vec3& a,
                                               const glm::vec3& b, const glm::vec3& c);

}  // namespace plant::voxelize
