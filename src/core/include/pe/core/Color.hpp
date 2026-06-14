#pragma once

#include <algorithm>
#include <cstdint>

namespace pe {

// Clamp a float to the normalized [0,1] range. NaN maps to 0 (a plain
// comparison clamp would let NaN pass through and poison downstream math).
[[nodiscard]] constexpr float clamp01(float v) noexcept {
    if (v != v) return 0.0f;  // NaN
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// 8-bit straight (non-premultiplied) RGBA. This is the storage format for the
// initial 8-bit pixel path. 16-bit and 32-bit-float buffers arrive in later
// milestones (see docs/15-color-management.md) but share this interface shape.
struct Rgba8 {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;

    constexpr bool operator==(const Rgba8&) const = default;
};

// Normalized floating-point RGBA, the working format for all blending and
// filtering math. Compositing happens in float to avoid banding/rounding.
struct Rgbaf {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

[[nodiscard]] constexpr float toLinearUnit(uint8_t v) noexcept {
    return static_cast<float>(v) / 255.0f;
}

[[nodiscard]] constexpr uint8_t fromUnit(float v) noexcept {
    if (v != v) return 0;  // NaN guard: static_cast<uint8_t>(NaN) is UB
    // Round-to-nearest, then clamp.
    const float s = clamp01(v) * 255.0f + 0.5f;
    return static_cast<uint8_t>(s);
}

[[nodiscard]] constexpr Rgbaf toFloat(Rgba8 c) noexcept {
    return Rgbaf{toLinearUnit(c.r), toLinearUnit(c.g), toLinearUnit(c.b), toLinearUnit(c.a)};
}

[[nodiscard]] constexpr Rgba8 toRgba8(Rgbaf c) noexcept {
    return Rgba8{fromUnit(c.r), fromUnit(c.g), fromUnit(c.b), fromUnit(c.a)};
}

// Premultiply colour by its own alpha. Premultiplied alpha is the correct space
// for compositing many layers; see docs/04-blend-modes via systems doc.
[[nodiscard]] constexpr Rgbaf premultiply(Rgbaf c) noexcept {
    return Rgbaf{c.r * c.a, c.g * c.a, c.b * c.a, c.a};
}

[[nodiscard]] constexpr Rgbaf unpremultiply(Rgbaf c) noexcept {
    if (c.a <= 0.0f) return Rgbaf{0.0f, 0.0f, 0.0f, 0.0f};
    const float inv = 1.0f / c.a;
    return Rgbaf{c.r * inv, c.g * inv, c.b * inv, c.a};
}

}  // namespace pe
