#include "pe/core/BlendMode.hpp"

#include <cmath>

namespace pe {

const char* blendModeName(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Normal:
            return "Normal";
        case BlendMode::Multiply:
            return "Multiply";
        case BlendMode::Screen:
            return "Screen";
        case BlendMode::Overlay:
            return "Overlay";
        case BlendMode::Darken:
            return "Darken";
        case BlendMode::Lighten:
            return "Lighten";
        case BlendMode::ColorDodge:
            return "Color Dodge";
        case BlendMode::ColorBurn:
            return "Color Burn";
        case BlendMode::HardLight:
            return "Hard Light";
        case BlendMode::SoftLight:
            return "Soft Light";
        case BlendMode::Difference:
            return "Difference";
        case BlendMode::Exclusion:
            return "Exclusion";
        case BlendMode::Count:
            return "?";
    }
    return "?";
}

namespace {

// W3C-compatible soft-light helper.
float softLight(float b, float s) noexcept {
    if (s <= 0.5f) {
        return b - (1.0f - 2.0f * s) * b * (1.0f - b);
    }
    const float d = (b <= 0.25f) ? ((16.0f * b - 12.0f) * b + 4.0f) * b : std::sqrt(b);
    return b + (2.0f * s - 1.0f) * (d - b);
}

}  // namespace

float blendChannel(BlendMode mode, float b, float s) noexcept {
    switch (mode) {
        case BlendMode::Normal:
            return s;
        case BlendMode::Multiply:
            return b * s;
        case BlendMode::Screen:
            return b + s - b * s;
        case BlendMode::Overlay:
            // Overlay(b,s) == HardLight(s,b)
            return (b <= 0.5f) ? (2.0f * b * s) : (1.0f - 2.0f * (1.0f - b) * (1.0f - s));
        case BlendMode::Darken:
            return b < s ? b : s;
        case BlendMode::Lighten:
            return b > s ? b : s;
        case BlendMode::ColorDodge:
            if (b <= 0.0f) return 0.0f;
            if (s >= 1.0f) return 1.0f;
            return clamp01(b / (1.0f - s));
        case BlendMode::ColorBurn:
            if (b >= 1.0f) return 1.0f;
            if (s <= 0.0f) return 0.0f;
            return 1.0f - clamp01((1.0f - b) / s);
        case BlendMode::HardLight:
            return (s <= 0.5f) ? (2.0f * b * s) : (1.0f - 2.0f * (1.0f - b) * (1.0f - s));
        case BlendMode::SoftLight:
            return softLight(b, s);
        case BlendMode::Difference:
            return std::fabs(b - s);
        case BlendMode::Exclusion:
            return b + s - 2.0f * b * s;
        case BlendMode::Count:
            return s;
    }
    return s;
}

Rgbaf compositeOver(BlendMode mode, Rgbaf backdrop, Rgbaf source, float opacity) noexcept {
    // Apply layer opacity to the source alpha.
    const float sa = clamp01(source.a) * clamp01(opacity);
    const float ba = clamp01(backdrop.a);

    // Per-channel blended colour (straight alpha inputs to blendChannel).
    const float br = clamp01(backdrop.r);
    const float bg = clamp01(backdrop.g);
    const float bb = clamp01(backdrop.b);

    const float blendedR = blendChannel(mode, br, clamp01(source.r));
    const float blendedG = blendChannel(mode, bg, clamp01(source.g));
    const float blendedB = blendChannel(mode, bb, clamp01(source.b));

    // Porter-Duff "over" with blend mode applied where backdrop is opaque.
    // out = src_over with the blended colour weighted by backdrop coverage:
    //   Cs' = (1 - ba) * Cs + ba * blend(Cb, Cs)
    // then standard source-over alpha composite.
    const float mixR = (1.0f - ba) * clamp01(source.r) + ba * blendedR;
    const float mixG = (1.0f - ba) * clamp01(source.g) + ba * blendedG;
    const float mixB = (1.0f - ba) * clamp01(source.b) + ba * blendedB;

    const float outA = sa + ba * (1.0f - sa);
    if (outA <= 0.0f) return Rgbaf{0.0f, 0.0f, 0.0f, 0.0f};

    // Composite premultiplied, then return straight alpha.
    const float invOutA = 1.0f / outA;
    Rgbaf out;
    out.r = (mixR * sa + br * ba * (1.0f - sa)) * invOutA;
    out.g = (mixG * sa + bg * ba * (1.0f - sa)) * invOutA;
    out.b = (mixB * sa + bb * ba * (1.0f - sa)) * invOutA;
    out.a = outA;
    return out;
}

}  // namespace pe
