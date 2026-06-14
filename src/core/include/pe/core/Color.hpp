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

// 16-bit straight (non-premultiplied) RGBA. The high-bit-depth storage format
// (docs/15-color-management.md): defends against banding and enables HDR. Shares
// the Rgba8/Rgbaf interface shape; conversions below are exact where possible.
struct Rgba16 {
    uint16_t r = 0;
    uint16_t g = 0;
    uint16_t b = 0;
    uint16_t a = 0;

    constexpr bool operator==(const Rgba16&) const = default;
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

[[nodiscard]] constexpr float toUnit16(uint16_t v) noexcept {
    return static_cast<float>(v) / 65535.0f;
}

[[nodiscard]] constexpr uint16_t fromUnit16(float v) noexcept {
    if (v != v) return 0;  // NaN guard: static_cast<uint16_t>(NaN) is UB
    const float s = clamp01(v) * 65535.0f + 0.5f;
    return static_cast<uint16_t>(s);
}

[[nodiscard]] constexpr Rgbaf toFloat(Rgba16 c) noexcept {
    return Rgbaf{toUnit16(c.r), toUnit16(c.g), toUnit16(c.b), toUnit16(c.a)};
}

[[nodiscard]] constexpr Rgba16 toRgba16(Rgbaf c) noexcept {
    return Rgba16{fromUnit16(c.r), fromUnit16(c.g), fromUnit16(c.b), fromUnit16(c.a)};
}

// 8-bit -> 16-bit is exact bit-replication (v * 257 maps 0->0 and 255->65535 with
// no gaps), so widening then narrowing round-trips losslessly.
[[nodiscard]] constexpr Rgba16 to16(Rgba8 c) noexcept {
    constexpr auto w = [](uint8_t v) { return static_cast<uint16_t>(v * 257); };
    return Rgba16{w(c.r), w(c.g), w(c.b), w(c.a)};
}

// 16-bit -> 8-bit is rounding division by 257 (the inverse of the bit-replication
// widening). Computed in uint32 so the +128 bias cannot overflow near full scale.
[[nodiscard]] constexpr Rgba8 to8(Rgba16 c) noexcept {
    constexpr auto n = [](uint16_t v) {
        return static_cast<uint8_t>((static_cast<uint32_t>(v) + 128u) / 257u);
    };
    return Rgba8{n(c.r), n(c.g), n(c.b), n(c.a)};
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
