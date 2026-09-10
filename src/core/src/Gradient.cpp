#include "pe/core/Gradient.hpp"

#include <algorithm>
#include <cstddef>

namespace pe {

namespace {

[[nodiscard]] Rgbaf resolve(const GradientStop& s, Rgbaf foreground, Rgbaf background) noexcept {
    switch (s.source) {
        case StopColor::Foreground:
            // The stop's own alpha, the live colour's RGB. See the header: this is what makes
            // "foreground to transparent" one gradient rather than one per colour.
            return Rgbaf{foreground.r, foreground.g, foreground.b, s.color.a};
        case StopColor::Background:
            return Rgbaf{background.r, background.g, background.b, s.color.a};
        case StopColor::Fixed:
        default:
            return s.color;
    }
}

[[nodiscard]] Rgbaf mix(Rgbaf a, Rgbaf b, float t) noexcept {
    return Rgbaf{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
                 a.a + (b.a - a.a) * t};
}

[[nodiscard]] std::vector<GradientStop> blackToWhite() {
    return {GradientStop{0.0f, Rgbaf{0.0f, 0.0f, 0.0f, 1.0f}, StopColor::Fixed},
            GradientStop{1.0f, Rgbaf{1.0f, 1.0f, 1.0f, 1.0f}, StopColor::Fixed}};
}

}  // namespace

Gradient::Gradient() : stops_(blackToWhite()) {}

Gradient::Gradient(std::vector<GradientStop> stops) {
    setStops(std::move(stops));
}

void Gradient::setStops(std::vector<GradientStop> stops) {
    for (GradientStop& s : stops) s.position = clamp01(s.position);
    // Stable, so two stops at the same position keep the order they were given: that is how a
    // hard edge is written (two stops at 0.5), and sorting them arbitrarily would flip which
    // side of the edge is which.
    std::stable_sort(stops.begin(), stops.end(), [](const GradientStop& a, const GradientStop& b) {
        return a.position < b.position;
    });
    stops_ = stops.size() >= 2 ? std::move(stops) : blackToWhite();
}

Rgbaf Gradient::sample(float t, Rgbaf foreground, Rgbaf background) const noexcept {
    if (stops_.empty()) return Rgbaf{0.0f, 0.0f, 0.0f, 0.0f};  // unreachable; setStops guards
    const float x = clamp01(t);
    if (x <= stops_.front().position) return resolve(stops_.front(), foreground, background);
    for (std::size_t i = 1; i < stops_.size(); ++i) {
        const GradientStop& hi = stops_[i];
        if (x > hi.position) continue;
        const GradientStop& lo = stops_[i - 1];
        const float span = hi.position - lo.position;
        // Coincident stops are a hard edge, not a division by zero: land on the upper stop,
        // which is the side of the edge x has reached.
        if (span <= 0.0f) return resolve(hi, foreground, background);
        return mix(resolve(lo, foreground, background), resolve(hi, foreground, background),
                   (x - lo.position) / span);
    }
    return resolve(stops_.back(), foreground, background);
}

bool Gradient::isFixed() const noexcept {
    return std::all_of(stops_.begin(), stops_.end(),
                       [](const GradientStop& s) { return s.source == StopColor::Fixed; });
}

Gradient Gradient::twoStop(Rgbaf a, Rgbaf b) {
    return Gradient(
        {GradientStop{0.0f, a, StopColor::Fixed}, GradientStop{1.0f, b, StopColor::Fixed}});
}

Gradient Gradient::foregroundToBackground() {
    return Gradient({GradientStop{0.0f, Rgbaf{0.0f, 0.0f, 0.0f, 1.0f}, StopColor::Foreground},
                     GradientStop{1.0f, Rgbaf{0.0f, 0.0f, 0.0f, 1.0f}, StopColor::Background}});
}

Gradient Gradient::foregroundToTransparent() {
    return Gradient({GradientStop{0.0f, Rgbaf{0.0f, 0.0f, 0.0f, 1.0f}, StopColor::Foreground},
                     GradientStop{1.0f, Rgbaf{0.0f, 0.0f, 0.0f, 0.0f}, StopColor::Foreground}});
}

}  // namespace pe
