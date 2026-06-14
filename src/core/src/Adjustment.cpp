#include "pe/core/Adjustment.hpp"

#include <algorithm>
#include <cmath>

namespace pe {

namespace {
// Operate on color only; a fully transparent pixel has no color to adjust.
inline bool opaqueEnough(const Rgbaf& p) noexcept {
    return p.a > 0.0f;
}
}  // namespace

void BrightnessContrast::apply(std::span<Rgbaf> tile) const {
    const float contrastFactor = 1.0f + std::clamp(contrast_, -1.0f, 1.0f);
    const float b = brightness_;
    const auto map = [&](float v) { return clamp01((v - 0.5f) * contrastFactor + 0.5f + b); };
    for (Rgbaf& p : tile) {
        if (!opaqueEnough(p)) continue;
        p.r = map(p.r);
        p.g = map(p.g);
        p.b = map(p.b);
    }
}

float Levels::mapChannel(float v) const noexcept {
    float n;
    if (inWhite_ > inBlack_) {
        n = clamp01((v - inBlack_) / (inWhite_ - inBlack_));
    } else {
        n = v >= inWhite_ ? 1.0f : 0.0f;  // degenerate range -> hard threshold
    }
    const float g = gamma_ > 0.0f ? std::pow(n, 1.0f / gamma_) : n;
    return clamp01(outBlack_ + g * (outWhite_ - outBlack_));
}

void Levels::rebuild() {
    lut_.bake([this](float v) { return mapChannel(v); });
}

void Levels::apply(std::span<Rgbaf> tile) const {
    for (Rgbaf& p : tile) {
        if (!opaqueEnough(p)) continue;
        p.r = lut_.apply(p.r);
        p.g = lut_.apply(p.g);
        p.b = lut_.apply(p.b);
    }
}

void Curves::setPoints(std::vector<std::pair<float, float>> pts) {
    for (auto& [x, y] : pts) {
        x = clamp01(x);
        y = clamp01(y);
    }
    std::stable_sort(pts.begin(), pts.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    // Keep x strictly increasing (drop later duplicates at the same x).
    std::vector<std::pair<float, float>> clean;
    for (const auto& pt : pts) {
        if (clean.empty() || pt.first > clean.back().first) clean.push_back(pt);
    }
    if (clean.size() >= 2) points_ = std::move(clean);
    rebuild();
}

float Curves::evalCurve(float x) const noexcept {
    x = clamp01(x);
    if (points_.empty()) return x;
    if (x <= points_.front().first) return points_.front().second;
    if (x >= points_.back().first) return points_.back().second;
    for (std::size_t i = 0; i + 1 < points_.size(); ++i) {
        const auto& a = points_[i];
        const auto& b = points_[i + 1];
        if (x >= a.first && x <= b.first) {
            const float span = b.first - a.first;
            const float t = span > 0.0f ? (x - a.first) / span : 0.0f;
            return clamp01(a.second + (b.second - a.second) * t);
        }
    }
    return x;
}

void Curves::rebuild() {
    lut_.bake([this](float v) { return evalCurve(v); });
}

void Curves::apply(std::span<Rgbaf> tile) const {
    for (Rgbaf& p : tile) {
        if (!opaqueEnough(p)) continue;
        p.r = lut_.apply(p.r);
        p.g = lut_.apply(p.g);
        p.b = lut_.apply(p.b);
    }
}

void Invert::apply(std::span<Rgbaf> tile) const {
    for (Rgbaf& p : tile) {
        if (!opaqueEnough(p)) continue;
        p.r = 1.0f - p.r;
        p.g = 1.0f - p.g;
        p.b = 1.0f - p.b;
    }
}

namespace {

struct Hsl {
    float h = 0.0f;  // degrees [0,360)
    float s = 0.0f;  // [0,1]
    float l = 0.0f;  // [0,1]
};

Hsl rgbToHsl(float r, float g, float b) noexcept {
    const float mx = std::max(r, std::max(g, b));
    const float mn = std::min(r, std::min(g, b));
    Hsl out;
    out.l = (mx + mn) * 0.5f;
    const float d = mx - mn;
    if (d > 1e-6f) {
        out.s = out.l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
        if (mx == r) {
            out.h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        } else if (mx == g) {
            out.h = (b - r) / d + 2.0f;
        } else {
            out.h = (r - g) / d + 4.0f;
        }
        out.h *= 60.0f;
    }
    return out;
}

float hue2rgb(float p, float q, float t) noexcept {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

void hslToRgb(float hDeg, float s, float l, float& r, float& g, float& b) noexcept {
    if (s <= 1e-6f) {
        r = g = b = l;
        return;
    }
    const float h = hDeg / 360.0f;
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    const float p = 2.0f * l - q;
    r = hue2rgb(p, q, h + 1.0f / 3.0f);
    g = hue2rgb(p, q, h);
    b = hue2rgb(p, q, h - 1.0f / 3.0f);
}

float wrapHue(float h) noexcept {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    return h;
}

float luminance(float r, float g, float b) noexcept {
    return 0.299f * r + 0.587f * g + 0.114f * b;  // Rec.601 luma (encoded, M5)
}

}  // namespace

void Exposure::apply(std::span<Rgbaf> tile) const {
    const float gain = std::exp2(stops_);
    const float invGamma = 1.0f / gamma_;
    const auto map = [&](float v) {
        const float lit = std::max(0.0f, v * gain + offset_);
        return clamp01(std::pow(lit, invGamma));
    };
    for (Rgbaf& p : tile) {
        if (!opaqueEnough(p)) continue;
        p.r = map(p.r);
        p.g = map(p.g);
        p.b = map(p.b);
    }
}

void HueSaturation::apply(std::span<Rgbaf> tile) const {
    for (Rgbaf& p : tile) {
        if (!opaqueEnough(p)) continue;
        const Hsl c = rgbToHsl(p.r, p.g, p.b);
        float rr = 0.0f, gg = 0.0f, bb = 0.0f;
        if (colorize_) {
            hslToRgb(wrapHue(colorizeHue_), colorizeSat_, clamp01(c.l), rr, gg, bb);
        } else {
            const float h = wrapHue(c.h + hueShift_);
            const float s = clamp01(c.s * satScale_);
            const float l = clamp01(c.l + lightness_);
            hslToRgb(h, s, l, rr, gg, bb);
        }
        // Clamp on write (the codebase's NaN sink): HSL math can exceed [0,1] for
        // out-of-gamut input and must never emit NaN/out-of-range into the composite.
        p.r = clamp01(rr);
        p.g = clamp01(gg);
        p.b = clamp01(bb);
    }
}

void ChannelMixer::apply(std::span<Rgbaf> tile) const {
    for (Rgbaf& p : tile) {
        if (!opaqueEnough(p)) continue;
        const float r = p.r, g = p.g, b = p.b;
        if (mono_) {
            const float gray = clamp01(m_[0][0] * r + m_[0][1] * g + m_[0][2] * b + m_[0][3]);
            p.r = p.g = p.b = gray;
        } else {
            p.r = clamp01(m_[0][0] * r + m_[0][1] * g + m_[0][2] * b + m_[0][3]);
            p.g = clamp01(m_[1][0] * r + m_[1][1] * g + m_[1][2] * b + m_[1][3]);
            p.b = clamp01(m_[2][0] * r + m_[2][1] * g + m_[2][2] * b + m_[2][3]);
        }
    }
}

void GradientMap::apply(std::span<Rgbaf> tile) const {
    for (Rgbaf& p : tile) {
        if (!opaqueEnough(p)) continue;
        float lum = luminance(p.r, p.g, p.b);
        if (reverse_) lum = 1.0f - lum;
        p.r = clamp01(color0_.r + (color1_.r - color0_.r) * lum);
        p.g = clamp01(color0_.g + (color1_.g - color0_.g) * lum);
        p.b = clamp01(color0_.b + (color1_.b - color0_.b) * lum);
    }
}

void Vibrance::apply(std::span<Rgbaf> tile) const {
    for (Rgbaf& p : tile) {
        if (!opaqueEnough(p)) continue;
        const Hsl c = rgbToHsl(p.r, p.g, p.b);
        // Vibrance boosts less-saturated colors more (the (1-s) factor) and protects
        // already-saturated ones; gray (s==0) is left gray.
        float s = c.s + vibrance_ * (1.0f - c.s) * c.s;
        s = clamp01(s * (1.0f + saturation_));  // linear saturation on top
        float rr = 0.0f, gg = 0.0f, bb = 0.0f;
        hslToRgb(c.h, s, c.l, rr, gg, bb);
        p.r = clamp01(rr);
        p.g = clamp01(gg);
        p.b = clamp01(bb);
    }
}

void ColorBalance::apply(std::span<Rgbaf> tile) const {
    for (Rgbaf& p : tile) {
        if (!opaqueEnough(p)) continue;
        const float origLum = luminance(p.r, p.g, p.b);
        const float shadowW = clamp01(1.0f - origLum * 2.0f);  // strong near black
        const float highW = clamp01((origLum - 0.5f) * 2.0f);  // strong near white
        const float midW = clamp01(1.0f - shadowW - highW);    // peak at mid-gray
        const auto shift = [&](int ch, float v) {
            const float s = shadows_[ch] * shadowW + midtones_[ch] * midW + highlights_[ch] * highW;
            return clamp01(v + s * 0.5f);  // modest strength
        };
        float r = shift(0, p.r);
        float g = shift(1, p.g);
        float b = shift(2, p.b);
        if (preserveLum_) {
            const float newLum = luminance(r, g, b);
            if (newLum > 1e-5f) {
                const float k = origLum / newLum;
                r = clamp01(r * k);
                g = clamp01(g * k);
                b = clamp01(b * k);
            }
        }
        p.r = r;
        p.g = g;
        p.b = b;
    }
}

}  // namespace pe
