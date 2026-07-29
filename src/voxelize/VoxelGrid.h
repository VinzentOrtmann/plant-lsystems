#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace plant::voxelize {

/// Palette slot. 0 is reserved: in the MagicaVoxel format it means "empty", so
/// a stored voxel always carries an index of 1 or more.
using ColorIndex = std::uint8_t;

struct Voxel {
    glm::ivec3 position{0};
    ColorIndex color = 0;
};

/// Sparse voxel volume with a world-space placement.
///
/// Storage is a hash map keyed by linear index, which suits a plant: the
/// bounding box of a tree is mostly air, and a dense 256^3 array would be
/// 16 MB of which a fraction of a percent is occupied.
///
/// Voxel (0,0,0) occupies the world box [origin, origin + voxelSize)^3, so
/// index and world space are related by origin + (position + 0.5) * voxelSize
/// at voxel centres.
class VoxelGrid {
public:
    VoxelGrid() = default;
    VoxelGrid(const glm::ivec3& dimensions, const glm::vec3& origin, float voxelSize);

    [[nodiscard]] const glm::ivec3& dimensions() const { return dimensions_; }
    [[nodiscard]] const glm::vec3& origin() const { return origin_; }
    [[nodiscard]] float voxelSize() const { return voxelSize_; }

    [[nodiscard]] bool inBounds(const glm::ivec3& position) const;
    [[nodiscard]] glm::vec3 voxelCenter(const glm::ivec3& position) const;

    /// Out-of-bounds writes and a colour of 0 are ignored; use erase() to clear
    /// a voxel. Returns true if a voxel was written.
    bool set(const glm::ivec3& position, ColorIndex color);
    bool erase(const glm::ivec3& position);

    /// Returns 0 for empty or out-of-bounds voxels.
    [[nodiscard]] ColorIndex at(const glm::ivec3& position) const;
    [[nodiscard]] bool occupied(const glm::ivec3& position) const;

    [[nodiscard]] std::size_t voxelCount() const { return voxels_.size(); }
    [[nodiscard]] bool empty() const { return voxels_.empty(); }
    void clear() { voxels_.clear(); }

    /// Inclusive bounds of the occupied voxels. False if nothing is occupied.
    [[nodiscard]] bool occupiedBounds(glm::ivec3& lo, glm::ivec3& hi) const;

    /// All voxels, ordered by z, then y, then x. The hash map's own order is
    /// unspecified, so anything that has to be reproducible -- the exporter,
    /// the tests -- goes through here.
    [[nodiscard]] std::vector<Voxel> toVector() const;

private:
    friend std::size_t blit(VoxelGrid&, const VoxelGrid&, const glm::ivec3&);

    [[nodiscard]] std::uint32_t indexOf(const glm::ivec3& position) const;
    [[nodiscard]] glm::ivec3 positionOf(std::uint32_t index) const;

    glm::ivec3 dimensions_{0};
    glm::vec3 origin_{0.0f};
    float voxelSize_ = 1.0f;
    std::unordered_map<std::uint32_t, ColorIndex> voxels_;
};

/// Copies every voxel of `source` into `destination`, shifted by `offset`
/// voxels. Anything landing outside `destination` is dropped rather than
/// clamped, exactly as set() behaves. Returns how many were written.
///
/// Both grids must share a voxel size for the result to mean anything; that is
/// the caller's business, since a merged grid has one origin and one scale.
std::size_t blit(VoxelGrid& destination, const VoxelGrid& source, const glm::ivec3& offset);

}  // namespace plant::voxelize
