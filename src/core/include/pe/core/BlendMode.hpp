#pragma once

#include "pe/core/Color.hpp"

#include <cstdint>

namespace pe {

// The compositor's blend-mode set. The numeric values are part of the on-disk
// document format, so existing values must never be reordered or reused.
// New modes append at the end. See docs/systems/04-blend-modes.md.
enum class BlendMode : uint8_t {
    Normal = 0,
    Multiply = 1,
    Screen = 2,
    Overlay = 3,
    Darken = 4,
    Lighten = 5,
    ColorDodge = 6,
    ColorBurn = 7,
    HardLight = 8,
    SoftLight = 9,
    Difference = 10,
    Exclusion = 11,
    // Separable modes end here. Non-separable (Hue/Saturation/Color/Luminosity)
    // operate on whole pixels and are added in a later milestone.

    Count
};

// Human-readable name, stable for UI and serialization debugging.
[[nodiscard]] const char* blendModeName(BlendMode mode) noexcept;

// Blend a single normalized channel (separable modes only). 'b' is the backdrop
// channel value, 's' is the source channel value, both in [0,1].
[[nodiscard]] float blendChannel(BlendMode mode, float b, float s) noexcept;

// Composite a straight-alpha source pixel over a straight-alpha backdrop using
// the given blend mode and a layer opacity in [0,1]. Returns straight alpha.
//
// This is the per-pixel kernel at the heart of the layer compositor. It is
// written for clarity here; the production path will be a SIMD/GPU version with
// identical semantics (validated against this reference by the test suite).
[[nodiscard]] Rgbaf compositeOver(BlendMode mode, Rgbaf backdrop, Rgbaf source,
                                  float opacity) noexcept;

} // namespace pe
