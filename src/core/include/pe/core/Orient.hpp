#pragma once

#include "pe/core/Geometry.hpp"

#include <cstdint>

namespace pe {

// The five exact (lossless) reorientations behind Image > Image Rotation. Each is a permutation of
// the pixels about the canvas frame, so unlike a resample they lose nothing. The 90-degree turns
// swap the canvas sides; the flips and the half-turn keep them.
enum class Orient : std::uint8_t {
    FlipHorizontal,  // mirror left<->right
    FlipVertical,    // mirror top<->bottom
    Rotate180,       // half turn
    Rotate90CW,      // quarter turn clockwise
    Rotate90CCW,     // quarter turn counter-clockwise
};

// The canvas size after `op` (90-degree turns swap width and height).
[[nodiscard]] Size orientedCanvas(Orient op, Size canvas) noexcept;

// Forward-map a document point: where the pixel at `src` lands after `op`, about a `canvas`-sized
// frame. Defined for any point, on- or off-canvas, so content outside the canvas turns with it.
[[nodiscard]] Point orientForward(Orient op, Size canvas, Point src) noexcept;

// Inverse-map a destination point back to its source; the exact reverse of orientForward.
[[nodiscard]] Point orientInverse(Orient op, Size canvas, Point dst) noexcept;

// The bounding box that source rect `r` maps to under orientForward (empty in, empty out).
[[nodiscard]] Rect orientRect(Orient op, Size canvas, Rect r) noexcept;

}  // namespace pe
