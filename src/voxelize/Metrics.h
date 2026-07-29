#pragma once

#include "voxelize/VoxelGrid.h"

#include <cstddef>
#include <vector>

/// Measurements over a finished voxel model.
///
/// The point of these is comparison: a single plant's numbers say little, but
/// sweeping a grammar parameter and watching one of them move is an experiment.
namespace plant::voxelize {

struct BoxCount {
    /// Edge length of the covering box, in voxels. Always a power of two.
    int boxSize = 1;
    /// How many boxes of that size contain at least one filled voxel.
    std::size_t boxes = 0;
};

/// Counts occupied boxes at successively doubled sizes, starting at one voxel
/// and stopping once a single box could swallow the whole model.
///
/// Boxes are anchored to the model's own bounding corner rather than the
/// grid's, so the result does not depend on where padding happened to leave it.
[[nodiscard]] std::vector<BoxCount> boxCounts(const VoxelGrid& grid);

/// Box-counting (Minkowski) dimension: the slope of log(boxes) against
/// log(1/boxSize), by least squares.
///
/// A filled solid measures 3, a flat sheet 2, a line 1; a branching plant lands
/// in between, and how far in between is a compact description of how densely
/// it fills the space it occupies. See chapter 8 of The Algorithmic Beauty of
/// Plants.
///
/// Returns 0 for a model too small to have a scaling range at all.
[[nodiscard]] double boxCountingDimension(const VoxelGrid& grid);

/// The dimension from counts already gathered, so a caller that wants to plot
/// the underlying points does not have to walk the grid twice.
[[nodiscard]] double boxCountingDimension(const std::vector<BoxCount>& counts);

}  // namespace plant::voxelize
