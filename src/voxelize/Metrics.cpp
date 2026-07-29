#include "voxelize/Metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace plant::voxelize {
namespace {

std::uint64_t packCell(const glm::ivec3& cell) {
    // 21 bits per axis covers a two-million-voxel edge, far past anything the
    // rasterizer will produce.
    return (static_cast<std::uint64_t>(cell.x) << 42) |
           (static_cast<std::uint64_t>(cell.y) << 21) | static_cast<std::uint64_t>(cell.z);
}

/// Points where the count has collapsed towards one box carry no information
/// about scaling -- log(1) is zero however fractal the model is -- and would
/// drag the fitted slope down. Likewise a box as large as the model itself.
bool usableForFit(const BoxCount& count, int largestAxis) {
    return count.boxes >= 4 && count.boxSize * 2 <= largestAxis;
}

}  // namespace

std::vector<BoxCount> boxCounts(const VoxelGrid& grid) {
    std::vector<BoxCount> counts;
    if (grid.empty()) {
        return counts;
    }

    // Fetched once: toVector sorts, and repeating that per box size would
    // dominate the measurement.
    const std::vector<Voxel> voxels = grid.toVector();

    // Boxes are anchored to the model's own corner, not the grid's. Otherwise
    // the same shape measures differently depending on where the padding
    // happens to have put it, because at coarse sizes it straddles a different
    // number of cell boundaries.
    glm::ivec3 low{0};
    glm::ivec3 high{0};
    if (!grid.occupiedBounds(low, high)) {
        return counts;
    }
    const glm::ivec3 extent = high - low + 1;
    const int largestAxis = std::max({extent.x, extent.y, extent.z, 1});

    std::unordered_set<std::uint64_t> occupied;
    for (int boxSize = 1; boxSize <= largestAxis; boxSize *= 2) {
        occupied.clear();
        occupied.reserve(voxels.size());
        for (const Voxel& voxel : voxels) {
            occupied.insert(packCell((voxel.position - low) / boxSize));
        }
        counts.push_back({boxSize, occupied.size()});
    }
    return counts;
}

double boxCountingDimension(const std::vector<BoxCount>& counts) {
    if (counts.empty()) {
        return 0.0;
    }
    const int largestAxis = counts.back().boxSize;

    // Least squares over log(1/boxSize) against log(boxes). The slope is the
    // dimension; the intercept is of no interest.
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXY = 0.0;
    double sumXX = 0.0;
    int used = 0;
    for (const BoxCount& count : counts) {
        if (!usableForFit(count, largestAxis)) {
            continue;
        }
        const double x = -std::log(static_cast<double>(count.boxSize));
        const double y = std::log(static_cast<double>(count.boxes));
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumXX += x * x;
        ++used;
    }
    if (used < 2) {
        return 0.0;  // no scaling range: too few voxels, or too coarse a grid
    }

    const double denominator = used * sumXX - sumX * sumX;
    if (std::fabs(denominator) < 1e-12) {
        return 0.0;
    }
    return (used * sumXY - sumX * sumY) / denominator;
}

double boxCountingDimension(const VoxelGrid& grid) {
    return boxCountingDimension(boxCounts(grid));
}

}  // namespace plant::voxelize
