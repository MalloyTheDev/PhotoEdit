#include "pe/core/ColorSpace.hpp"

#include <cmath>

namespace pe {

// sRGB transfer function constants (IEC 61966-2-1).
namespace {
constexpr float kSrgbThreshLinear = 0.0031308f;  // linear-side knee
constexpr float kSrgbThreshGamma = 0.04045f;     // gamma-side knee
constexpr float kSrgbSlope = 12.92f;             // linear-segment slope
constexpr float kSrgbAlpha = 1.055f;
constexpr float kSrgbOffset = 0.055f;
constexpr float kSrgbGamma = 2.4f;
}  // namespace

float srgbToLinear(float c) noexcept {
    // The branch on the knee keeps the power function away from negative bases:
    // anything <= the knee (including all negatives) uses the linear segment, so no
    // std::pow of a negative value is ever evaluated -> no NaN.
    if (c <= kSrgbThreshGamma) return c / kSrgbSlope;
    return std::pow((c + kSrgbOffset) / kSrgbAlpha, kSrgbGamma);
}

float linearToSrgb(float c) noexcept {
    if (c <= kSrgbThreshLinear) return c * kSrgbSlope;  // negatives included -> no pow
    return kSrgbAlpha * std::pow(c, 1.0f / kSrgbGamma) - kSrgbOffset;
}

Rgbaf toLinear(Rgbaf p) noexcept {
    return Rgbaf{srgbToLinear(p.r), srgbToLinear(p.g), srgbToLinear(p.b), p.a};
}

Rgbaf toGammaSrgb(Rgbaf p) noexcept {
    return Rgbaf{linearToSrgb(p.r), linearToSrgb(p.g), linearToSrgb(p.b), p.a};
}

}  // namespace pe
