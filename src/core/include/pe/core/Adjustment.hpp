#pragma once

#include "pe/core/Color.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pe {

enum class AdjustmentKind : uint8_t {
    BrightnessContrast = 0,
    Levels,
    Curves,
    Invert,
    // Exposure, Vibrance, HueSaturation, ColorBalance, ... arrive in later increments.
};

// A baked 256-entry 1D lookup table over the [0,1] domain (exact for 8-bit input),
// with linear interpolation for in-between (16/32-bit) values. The per-pixel kernel
// for tone ops becomes a single table fetch.
class Lut1D {
public:
    template <class Fn>
    void bake(Fn fn) {
        for (int i = 0; i < kSize; ++i) {
            table_[static_cast<std::size_t>(i)] = clamp01(fn(static_cast<float>(i) / 255.0f));
        }
    }

    [[nodiscard]] float apply(float v) const noexcept {
        const float x = clamp01(v) * 255.0f;
        const int i = static_cast<int>(x);
        if (i >= 255) return table_[255];
        const float frac = x - static_cast<float>(i);
        const float a = table_[static_cast<std::size_t>(i)];
        const float b = table_[static_cast<std::size_t>(i) + 1];
        return a + (b - a) * frac;
    }

private:
    static constexpr int kSize = 256;
    std::array<float, kSize> table_{};
};

// The interface every adjustment satisfies: stored parameters + an apply over a tile
// of straight-alpha working-space float pixels, transforming color in place. Fully
// transparent pixels (alpha 0) are left unchanged (no color to adjust). region and
// working-space awareness are added in later increments; M5 core ops are per-pixel.
class Adjustment {
public:
    virtual ~Adjustment() = default;
    [[nodiscard]] virtual AdjustmentKind kind() const noexcept = 0;
    [[nodiscard]] virtual std::string name() const = 0;
    virtual void apply(std::span<Rgbaf> tile) const = 0;
    [[nodiscard]] virtual std::unique_ptr<Adjustment> clone() const = 0;
};

// Brightness/Contrast: shift then scale around mid-gray. brightness/contrast in [-1,1].
class BrightnessContrast final : public Adjustment {
public:
    explicit BrightnessContrast(float brightness = 0.0f, float contrast = 0.0f)
        : brightness_(std::clamp(brightness, -1.0f, 1.0f)),
          contrast_(std::clamp(contrast, -1.0f, 1.0f)) {}

    void setBrightness(float b) noexcept { brightness_ = std::clamp(b, -1.0f, 1.0f); }
    void setContrast(float c) noexcept { contrast_ = std::clamp(c, -1.0f, 1.0f); }
    [[nodiscard]] float brightness() const noexcept { return brightness_; }
    [[nodiscard]] float contrast() const noexcept { return contrast_; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override {
        return AdjustmentKind::BrightnessContrast;
    }
    [[nodiscard]] std::string name() const override { return "Brightness/Contrast"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<BrightnessContrast>(*this);
    }

private:
    float brightness_;
    float contrast_;
};

// Levels: input black/white + gamma mapped to an output range, on the composite RGB
// channel (per-channel arrives with the channel set in M6). Baked to a 1D LUT.
class Levels final : public Adjustment {
public:
    // The LUT is baked eagerly in the constructor and on every setter, so apply()
    // is read-only and therefore safe for the future multithreaded compositor.
    Levels() { rebuild(); }

    void setInputBlack(float v) { inBlack_ = clamp01(v), rebuild(); }
    void setInputWhite(float v) { inWhite_ = clamp01(v), rebuild(); }
    void setGamma(float g) { gamma_ = g, rebuild(); }
    void setOutputBlack(float v) { outBlack_ = clamp01(v), rebuild(); }
    void setOutputWhite(float v) { outWhite_ = clamp01(v), rebuild(); }

    [[nodiscard]] AdjustmentKind kind() const noexcept override { return AdjustmentKind::Levels; }
    [[nodiscard]] std::string name() const override { return "Levels"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<Levels>(*this);
    }

    [[nodiscard]] float mapChannel(float v) const noexcept;  // reference (analytic)

private:
    void rebuild();

    float inBlack_ = 0.0f, inWhite_ = 1.0f, gamma_ = 1.0f, outBlack_ = 0.0f, outWhite_ = 1.0f;
    Lut1D lut_;
};

// Curves: a monotone piecewise-linear tone curve (>=2 x-monotonic points in [0,1])
// on the composite channel, baked to a 1D LUT. Spline smoothing arrives later.
class Curves final : public Adjustment {
public:
    Curves() : points_{{0.0f, 0.0f}, {1.0f, 1.0f}} { rebuild(); }

    // Replace the control points (sorted/sanitized to be x-monotonic on set). Bakes
    // the LUT immediately, so apply() stays read-only (thread-safe).
    void setPoints(std::vector<std::pair<float, float>> pts);
    [[nodiscard]] const std::vector<std::pair<float, float>>& points() const noexcept {
        return points_;
    }

    [[nodiscard]] AdjustmentKind kind() const noexcept override { return AdjustmentKind::Curves; }
    [[nodiscard]] std::string name() const override { return "Curves"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<Curves>(*this);
    }

    [[nodiscard]] float evalCurve(float x) const noexcept;  // reference (analytic)

private:
    void rebuild();

    std::vector<std::pair<float, float>> points_;
    Lut1D lut_;
};

// Invert: out = 1 - in per channel.
class Invert final : public Adjustment {
public:
    [[nodiscard]] AdjustmentKind kind() const noexcept override { return AdjustmentKind::Invert; }
    [[nodiscard]] std::string name() const override { return "Invert"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<Invert>(*this);
    }
};

}  // namespace pe
