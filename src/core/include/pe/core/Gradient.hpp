#pragma once

#include "pe/core/Color.hpp"

#include <cstdint>
#include <vector>

namespace pe {

// Where a stop takes its colour from.
//
// A preset has to keep working when the loaded colours change: "Foreground to Transparent" is
// one gradient, not one per colour anyone might pick. A Fixed stop carries its own colour; the
// other two follow whatever is loaded at the moment the gradient is drawn. Photoshop's gradient
// editor offers exactly these three for the same reason.
enum class StopColor : std::uint8_t { Fixed = 0, Foreground, Background };

// One stop along the axis.
//
// `color.a` always applies, including on a Foreground or Background stop, which is what makes
// "foreground to transparent" expressible: two Foreground stops whose alphas run 1 to 0. Only
// the RGB is taken from the live colour.
struct GradientStop {
    float position = 0.0f;  // [0,1] along start -> end
    Rgbaf color{0.0f, 0.0f, 0.0f, 1.0f};
    StopColor source = StopColor::Fixed;

    [[nodiscard]] constexpr bool operator==(const GradientStop&) const noexcept = default;
};

// A colour ramp with any number of stops, sampled along a normalized axis.
//
// The engine's gradient fill was two colours end to end, which is the one gradient that needs
// no presets at all. Everything a Gradients panel would hold (a sunset, a metal ramp, anything
// fading to transparent in the middle) needs stops, so the stops live here rather than in the
// shell: the fill, the panel's preview swatches and any future gradient map all read one model,
// and cannot disagree about what a gradient looks like.
//
// Sampling is a linear scan over the stops. A baked LUT would be faster per pixel and is what
// the tone curves use, but a gradient is drawn across a whole canvas rather than a 0..255
// domain, and 256 entries would band visibly across a few thousand pixels. Presets carry a
// handful of stops, so the scan costs a few comparisons.
class Gradient {
public:
    Gradient();  // black to white, the neutral default
    explicit Gradient(std::vector<GradientStop> stops);

    [[nodiscard]] static Gradient twoStop(Rgbaf a, Rgbaf b);
    [[nodiscard]] static Gradient foregroundToBackground();
    [[nodiscard]] static Gradient foregroundToTransparent();

    // Stops are sorted by position and clamped to [0,1] on the way in, so sample() can assume
    // order. Fewer than two stops is not a gradient; the ramp falls back to black to white
    // rather than leaving a shape that samples to nothing.
    void setStops(std::vector<GradientStop> stops);
    [[nodiscard]] const std::vector<GradientStop>& stops() const noexcept { return stops_; }

    // The colour at `t`, clamped to [0,1], with `foreground` and `background` substituted into
    // the stops that follow them. Before the first stop and after the last, the ramp holds that
    // stop's colour rather than fading out.
    [[nodiscard]] Rgbaf sample(float t, Rgbaf foreground, Rgbaf background) const noexcept;

    // True when no stop follows the loaded colours, so the ramp is the same whatever they are.
    // The panel uses it to decide whether a preview swatch has to be redrawn when the
    // foreground changes.
    [[nodiscard]] bool isFixed() const noexcept;

    [[nodiscard]] bool operator==(const Gradient&) const noexcept = default;

private:
    std::vector<GradientStop> stops_;
};

}  // namespace pe
