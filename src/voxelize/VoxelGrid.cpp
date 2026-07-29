#include "voxelize/VoxelGrid.h"

#include <glm/common.hpp>

#include <algorithm>

namespace plant::voxelize {

VoxelGrid::VoxelGrid(const glm::ivec3& dimensions, const glm::vec3& origin, float voxelSize)
    : dimensions_(glm::max(dimensions, glm::ivec3(0))), origin_(origin), voxelSize_(voxelSize) {}

bool VoxelGrid::inBounds(const glm::ivec3& position) const {
    return position.x >= 0 && position.y >= 0 && position.z >= 0 && position.x < dimensions_.x &&
           position.y < dimensions_.y && position.z < dimensions_.z;
}

glm::vec3 VoxelGrid::voxelCenter(const glm::ivec3& position) const {
    return origin_ + (glm::vec3(position) + 0.5f) * voxelSize_;
}

std::uint32_t VoxelGrid::indexOf(const glm::ivec3& position) const {
    return static_cast<std::uint32_t>((position.z * dimensions_.y + position.y) * dimensions_.x +
                                      position.x);
}

glm::ivec3 VoxelGrid::positionOf(std::uint32_t index) const {
    const auto slice = static_cast<std::uint32_t>(dimensions_.x * dimensions_.y);
    const auto z = static_cast<int>(index / slice);
    const std::uint32_t rest = index % slice;
    return {static_cast<int>(rest % static_cast<std::uint32_t>(dimensions_.x)),
            static_cast<int>(rest / static_cast<std::uint32_t>(dimensions_.x)), z};
}

bool VoxelGrid::set(const glm::ivec3& position, ColorIndex color) {
    if (color == 0 || !inBounds(position)) {
        return false;
    }
    voxels_[indexOf(position)] = color;
    return true;
}

bool VoxelGrid::erase(const glm::ivec3& position) {
    if (!inBounds(position)) {
        return false;
    }
    return voxels_.erase(indexOf(position)) > 0;
}

ColorIndex VoxelGrid::at(const glm::ivec3& position) const {
    if (!inBounds(position)) {
        return 0;
    }
    const auto found = voxels_.find(indexOf(position));
    return found != voxels_.end() ? found->second : ColorIndex{0};
}

bool VoxelGrid::occupied(const glm::ivec3& position) const {
    return at(position) != 0;
}

bool VoxelGrid::occupiedBounds(glm::ivec3& lo, glm::ivec3& hi) const {
    if (voxels_.empty()) {
        return false;
    }
    lo = dimensions_;
    hi = glm::ivec3(-1);
    for (const auto& [index, color] : voxels_) {
        const glm::ivec3 position = positionOf(index);
        lo = glm::min(lo, position);
        hi = glm::max(hi, position);
    }
    return true;
}

std::size_t blit(VoxelGrid& destination, const VoxelGrid& source, const glm::ivec3& offset) {
    std::size_t written = 0;
    for (const auto& [index, color] : source.voxels_) {
        if (destination.set(source.positionOf(index) + offset, color)) {
            ++written;
        }
    }
    return written;
}

std::vector<Voxel> VoxelGrid::toVector() const {
    std::vector<Voxel> result;
    result.reserve(voxels_.size());

    std::vector<std::uint32_t> indices;
    indices.reserve(voxels_.size());
    for (const auto& [index, color] : voxels_) {
        indices.push_back(index);
    }
    // The linear index is already z-major, so sorting it sorts by z, then y, then x.
    std::sort(indices.begin(), indices.end());

    for (const std::uint32_t index : indices) {
        result.push_back({positionOf(index), voxels_.at(index)});
    }
    return result;
}

}  // namespace plant::voxelize
