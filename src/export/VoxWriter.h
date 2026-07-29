#pragma once

#include "voxelize/VoxelGrid.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <vector>

/// MagicaVoxel .vox writer.
///
/// The format is a header followed by a tree of RIFF-style chunks, each one
/// `id[4] | int32 contentBytes | int32 childBytes | content | children`. A
/// single-model file needs three chunks under MAIN: SIZE, XYZI and RGBA.
///
/// Named `plant::vox` rather than `plant::export` because `export` is a
/// reserved word in C++20.
namespace plant::vox {

struct Rgba {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
};

/// Palette indexed the way voxels are: slot 0 is "empty" and never drawn, so
/// only 1..255 carry colour. The on-disk RGBA chunk is shifted down by one
/// relative to this, which the writer handles.
using Palette = std::array<Rgba, 256>;

/// MagicaVoxel stores voxel coordinates as single bytes, so no axis can exceed
/// this. RasterizerConfig::maxDimension is deliberately looser, to allow
/// previewing at a resolution higher than you can export.
inline constexpr int kMaxDimension = 256;

struct VoxWriteError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

/// Bark-to-leaf ramp occupying `colorCount` slots from `firstColor`, matching
/// the ramp RasterizerConfig assigns by branch depth, plus a distinct foliage
/// green in `polygonColor` for filled surfaces. Slots outside both get a
/// neutral grey so a hand-edited colour index still shows up.
[[nodiscard]] Palette barkToLeafPalette(voxelize::ColorIndex firstColor = 1, int colorCount = 8,
                                        voxelize::ColorIndex polygonColor = 9);

/// Serializes the grid into .vox bytes. Kept separate from the file write so
/// the format can be tested without touching the filesystem.
///
/// Throws VoxWriteError if any axis exceeds kMaxDimension.
[[nodiscard]] std::vector<std::uint8_t> encodeVox(const voxelize::VoxelGrid& grid,
                                                  const Palette& palette);

/// Writes encodeVox() to `path`, creating parent directories as needed.
void writeVox(const std::filesystem::path& path, const voxelize::VoxelGrid& grid,
              const Palette& palette);

}  // namespace plant::vox
