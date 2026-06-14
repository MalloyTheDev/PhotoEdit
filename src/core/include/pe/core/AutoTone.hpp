#pragma once

#include "pe/core/Color.hpp"
#include "pe/core/Histogram.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace pe {

class PixelBuffer;

// The Image > Auto Tone family. Each mode derives per-channel input black/white
// points from a histogram by clipping a small fraction of the darkest/brightest
// pixels, then stretches each channel's [black,white] range to full scale.
//
//  - Contrast: a single black/white pair (from the luma histogram) applied to all
//    three channels equally — boosts contrast without shifting the color balance.
//  - Levels:   per-channel black/white (from each channel's own histogram) — also
//    removes a color cast, but can shift hue (Photoshop's "Enhance Per Channel").
enum class AutoToneMode : uint8_t { Contrast, Levels };

// Per-channel input endpoints (0..255 levels) that the stretch maps to 0..255.
// A channel with whitePoint <= blackPoint is degenerate and left unmapped (the
// stretch is identity for it), so a flat image is never divided by zero.
struct AutoToneLevels {
    std::array<int, 3> blackPoint{0, 0, 0};
    std::array<int, 3> whitePoint{255, 255, 255};
};

// Derive the auto-tone endpoints from a histogram. clipFraction in [0,0.49] is the
// fraction of pixels clipped at EACH end (Photoshop's default is ~0.001). Values
// outside the range are clamped.
[[nodiscard]] AutoToneLevels computeAutoTone(const Histogram& hist, AutoToneMode mode,
                                             double clipFraction = 0.001);

// Apply a per-channel linear stretch to straight-alpha float pixels in place:
// out = clamp01((v - black)/(white - black)). Alpha is untouched; transparent
// pixels (alpha 0) are skipped (no color to stretch), matching the adjustments.
void applyAutoTone(std::span<Rgbaf> tile, const AutoToneLevels& levels);

}  // namespace pe
