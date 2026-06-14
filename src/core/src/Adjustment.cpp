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

}  // namespace pe
