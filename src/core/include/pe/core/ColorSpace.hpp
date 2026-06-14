#pragma once

#include "pe/core/Color.hpp"

#include <cstdint>

namespace pe {

// Whether stored color values are gamma-encoded (perceptual, right for storage and
// an 8-bit code budget) or in linear light (physically correct for operations that
// model photons: alpha compositing, blurs/convolutions, downscaling). The working
// space records this so the math knows what it is operating on.
// See docs/systems/15-color-management.md.
enum class Encoding : uint8_t { Linear, GammaEncoded };

// The IEC 61966-2-1 sRGB transfer function, per channel. Nominal domain is [0,1],
// but both directions are TOTAL and NaN-free for any finite input and preserve
// out-of-range values (negatives pass through the linear segment; values > 1 stay
// > 1), so scene-linear / HDR pipelines keep their highlights instead of clipping.
[[nodiscard]] float srgbToLinear(float c) noexcept;  // decode: gamma-encoded -> linear
[[nodiscard]] float linearToSrgb(float c) noexcept;  // encode: linear -> gamma-encoded

// Apply the transfer function to a straight-alpha pixel's RGB; alpha is unchanged
// (it is a coverage value, not a color, and is always linear).
[[nodiscard]] Rgbaf toLinear(Rgbaf p) noexcept;     // sRGB-encoded pixel -> linear light
[[nodiscard]] Rgbaf toGammaSrgb(Rgbaf p) noexcept;  // linear-light pixel -> sRGB-encoded

}  // namespace pe
