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
    Exposure,
    HueSaturation,
    ChannelMixer,
    GradientMap,
    Vibrance,
    ColorBalance,
    BlackAndWhite,
    PhotoFilter,
    Posterize,
    Threshold,
    SelectiveColor,
    // ColorLookup (1D/3D LUT) ... later.
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

    void setInputBlack(float v) {
        inBlack_ = clamp01(v);
        rebuild();
    }
    void setInputWhite(float v) {
        inWhite_ = clamp01(v);
        rebuild();
    }
    // Gamma is clamped to a sane range so a stray 0/negative can't divide by zero or
    // emit NaN, and absurd values can't produce a degenerate LUT.
    void setGamma(float g) {
        gamma_ = std::clamp(g, kMinGamma, kMaxGamma);
        rebuild();
    }
    void setOutputBlack(float v) {
        outBlack_ = clamp01(v);
        rebuild();
    }
    void setOutputWhite(float v) {
        outWhite_ = clamp01(v);
        rebuild();
    }

    [[nodiscard]] float inputBlack() const noexcept { return inBlack_; }
    [[nodiscard]] float inputWhite() const noexcept { return inWhite_; }
    [[nodiscard]] float gamma() const noexcept { return gamma_; }
    [[nodiscard]] float outputBlack() const noexcept { return outBlack_; }
    [[nodiscard]] float outputWhite() const noexcept { return outWhite_; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override { return AdjustmentKind::Levels; }
    [[nodiscard]] std::string name() const override { return "Levels"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<Levels>(*this);
    }

    [[nodiscard]] float mapChannel(float v) const noexcept;  // reference (analytic)

    static constexpr float kMinGamma = 0.1f;
    static constexpr float kMaxGamma = 9.99f;

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

// Exposure: scene gain (2^stops), then offset (lift), then a gamma. Applied to the
// encoded working values in M5 (linear-light correctness arrives in M6). Per-channel.
class Exposure final : public Adjustment {
public:
    explicit Exposure(float stops = 0.0f, float offset = 0.0f, float gamma = 1.0f)
        : stops_(stops), offset_(offset), gamma_(gamma > 0.0f ? gamma : 1.0f) {}
    void setStops(float s) noexcept { stops_ = s; }
    void setOffset(float o) noexcept { offset_ = o; }
    void setGamma(float g) noexcept { gamma_ = g > 0.0f ? g : 1.0f; }
    [[nodiscard]] float stops() const noexcept { return stops_; }
    [[nodiscard]] float offset() const noexcept { return offset_; }
    [[nodiscard]] float gamma() const noexcept { return gamma_; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override { return AdjustmentKind::Exposure; }
    [[nodiscard]] std::string name() const override { return "Exposure"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<Exposure>(*this);
    }

private:
    float stops_, offset_, gamma_;
};

// Hue/Saturation (master band): rotate hue, scale saturation, shift lightness in
// HSL. Colorize maps everything to one hue/saturation. Perceptual (cross-channel).
class HueSaturation final : public Adjustment {
public:
    void setHueShiftDegrees(float d) noexcept { hueShift_ = d; }
    void setSaturationScale(float s) noexcept { satScale_ = s < 0.0f ? 0.0f : s; }
    void setLightness(float l) noexcept { lightness_ = std::clamp(l, -1.0f, 1.0f); }
    void setColorize(bool on, float hueDeg = 0.0f, float sat = 0.5f) noexcept {
        colorize_ = on;
        colorizeHue_ = hueDeg;
        colorizeSat_ = clamp01(sat);
    }
    [[nodiscard]] float hueShiftDegrees() const noexcept { return hueShift_; }
    [[nodiscard]] float saturationScale() const noexcept { return satScale_; }
    [[nodiscard]] float lightness() const noexcept { return lightness_; }
    [[nodiscard]] bool colorize() const noexcept { return colorize_; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override {
        return AdjustmentKind::HueSaturation;
    }
    [[nodiscard]] std::string name() const override { return "Hue/Saturation"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<HueSaturation>(*this);
    }

private:
    float hueShift_ = 0.0f;   // degrees
    float satScale_ = 1.0f;   // multiplier
    float lightness_ = 0.0f;  // [-1,1] add to L
    bool colorize_ = false;
    float colorizeHue_ = 0.0f;  // degrees
    float colorizeSat_ = 0.5f;  // [0,1]
};

// Channel Mixer: each output channel is a linear combination of the inputs plus a
// constant; monochrome collapses to a single weighted gray. Cross-channel.
class ChannelMixer final : public Adjustment {
public:
    // m[out][0..2] = R,G,B coefficients; m[out][3] = constant. Defaults to identity.
    void setRow(int out, float r, float g, float b, float constant) noexcept {
        if (out < 0 || out > 2) return;
        m_[out][0] = r;
        m_[out][1] = g;
        m_[out][2] = b;
        m_[out][3] = constant;
    }
    void setMonochrome(bool on) noexcept { mono_ = on; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override {
        return AdjustmentKind::ChannelMixer;
    }
    [[nodiscard]] std::string name() const override { return "Channel Mixer"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<ChannelMixer>(*this);
    }

private:
    float m_[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    bool mono_ = false;
};

// Vibrance: a non-linear saturation boost that affects less-saturated colors more
// (protecting already-saturated ones), plus a linear saturation. Both in [-1,1].
class Vibrance final : public Adjustment {
public:
    explicit Vibrance(float vibrance = 0.0f, float saturation = 0.0f)
        : vibrance_(std::clamp(vibrance, -1.0f, 1.0f)),
          saturation_(std::clamp(saturation, -1.0f, 1.0f)) {}
    void setVibrance(float v) noexcept { vibrance_ = std::clamp(v, -1.0f, 1.0f); }
    void setSaturation(float s) noexcept { saturation_ = std::clamp(s, -1.0f, 1.0f); }
    [[nodiscard]] float vibrance() const noexcept { return vibrance_; }
    [[nodiscard]] float saturation() const noexcept { return saturation_; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override { return AdjustmentKind::Vibrance; }
    [[nodiscard]] std::string name() const override { return "Vibrance"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<Vibrance>(*this);
    }

private:
    float vibrance_, saturation_;
};

// Color Balance: shift colors per tonal range (shadows/midtones/highlights). Each
// range carries an R/G/B shift in [-1,1] (cyan-red, magenta-green, yellow-blue).
class ColorBalance final : public Adjustment {
public:
    void setShadows(float r, float g, float b) noexcept { set(shadows_, r, g, b); }
    void setMidtones(float r, float g, float b) noexcept { set(midtones_, r, g, b); }
    void setHighlights(float r, float g, float b) noexcept { set(highlights_, r, g, b); }
    void setPreserveLuminosity(bool on) noexcept { preserveLum_ = on; }
    // Per-channel read access (channel 0=R, 1=G, 2=B; out-of-range yields 0).
    [[nodiscard]] float shadow(int ch) const noexcept { return get(shadows_, ch); }
    [[nodiscard]] float midtone(int ch) const noexcept { return get(midtones_, ch); }
    [[nodiscard]] float highlight(int ch) const noexcept { return get(highlights_, ch); }
    [[nodiscard]] bool preserveLuminosity() const noexcept { return preserveLum_; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override {
        return AdjustmentKind::ColorBalance;
    }
    [[nodiscard]] std::string name() const override { return "Color Balance"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<ColorBalance>(*this);
    }

private:
    static void set(float (&dst)[3], float r, float g, float b) noexcept {
        dst[0] = std::clamp(r, -1.0f, 1.0f);
        dst[1] = std::clamp(g, -1.0f, 1.0f);
        dst[2] = std::clamp(b, -1.0f, 1.0f);
    }
    static float get(const float (&src)[3], int ch) noexcept {
        return (ch >= 0 && ch < 3) ? src[ch] : 0.0f;
    }
    float shadows_[3] = {0, 0, 0};
    float midtones_[3] = {0, 0, 0};
    float highlights_[3] = {0, 0, 0};
    bool preserveLum_ = true;
};

// Gradient Map: map each pixel's luminance to a two-stop gradient (color0..color1).
// Cross-channel; defaults to black->white (luminance to grayscale).
class GradientMap final : public Adjustment {
public:
    explicit GradientMap(Rgbaf color0 = Rgbaf{0, 0, 0, 1}, Rgbaf color1 = Rgbaf{1, 1, 1, 1})
        : color0_(color0), color1_(color1) {}
    void setColors(Rgbaf c0, Rgbaf c1) noexcept {
        color0_ = c0;
        color1_ = c1;
    }
    void setReverse(bool r) noexcept { reverse_ = r; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override {
        return AdjustmentKind::GradientMap;
    }
    [[nodiscard]] std::string name() const override { return "Gradient Map"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<GradientMap>(*this);
    }

private:
    Rgbaf color0_, color1_;
    bool reverse_ = false;
};

// Black & White: convert to grayscale with six per-hue band multipliers (reds,
// yellows, greens, cyans, blues, magentas) applied to each pixel's chroma. The
// achromatic part passes through; the chromatic part is weighted by the band for
// the pixel's hue. Bands in [-2,3] (-200%..+300%, as in Photoshop). Cross-channel.
class BlackAndWhite final : public Adjustment {
public:
    enum Band { Reds = 0, Yellows, Greens, Cyans, Blues, Magentas, kBandCount };

    BlackAndWhite() = default;

    // Set one band's chroma multiplier; clamped to the Photoshop range [-2,3].
    void setBand(int band, float weight) noexcept {
        if (band < 0 || band >= kBandCount) return;
        bands_[static_cast<std::size_t>(band)] = std::clamp(weight, -2.0f, 3.0f);
    }
    [[nodiscard]] float band(int b) const noexcept {
        return (b < 0 || b >= kBandCount) ? 0.0f : bands_[static_cast<std::size_t>(b)];
    }

    [[nodiscard]] AdjustmentKind kind() const noexcept override {
        return AdjustmentKind::BlackAndWhite;
    }
    [[nodiscard]] std::string name() const override { return "Black & White"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<BlackAndWhite>(*this);
    }

private:
    // Photoshop's default mix (reds .40, yellows .60, greens .40, cyans .60,
    // blues .20, magentas .80) — the chroma multiplier per hue band.
    std::array<float, kBandCount> bands_{0.40f, 0.60f, 0.40f, 0.60f, 0.20f, 0.80f};
};

// Photo Filter: tint the image toward a filter color (multiply blend) by a density
// in [0,1], optionally preserving the original luminosity. Models warming/cooling
// and custom-color filters. Cross-channel when preserving luminosity.
class PhotoFilter final : public Adjustment {
public:
    // Default is the "Warming Filter (85)" color at 25% density, matching Photoshop.
    explicit PhotoFilter(Rgbaf color = Rgbaf{236.0f / 255.0f, 138.0f / 255.0f, 0.0f, 1.0f},
                         float density = 0.25f)
        : color_(sanitizeColor(color)), density_(clamp01(density)) {}
    void setColor(Rgbaf c) noexcept { color_ = sanitizeColor(c); }
    void setDensity(float d) noexcept { density_ = clamp01(d); }
    void setPreserveLuminosity(bool on) noexcept { preserveLum_ = on; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override {
        return AdjustmentKind::PhotoFilter;
    }
    [[nodiscard]] std::string name() const override { return "Photo Filter"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<PhotoFilter>(*this);
    }

private:
    // A multiply-blend filter color outside [0,1] is meaningless and a NaN channel
    // would silently poison the tint (sunk to black on write); clamp it on the way in,
    // matching the discipline density_ already follows.
    static Rgbaf sanitizeColor(Rgbaf c) noexcept {
        return Rgbaf{clamp01(c.r), clamp01(c.g), clamp01(c.b), clamp01(c.a)};
    }
    Rgbaf color_;
    float density_;
    bool preserveLum_ = true;
};

// Posterize: quantize each channel to N evenly spaced levels in [2,255]. Baked to a
// 1D LUT, so apply() is a single read-only table fetch (thread-safe). Per-channel.
class Posterize final : public Adjustment {
public:
    explicit Posterize(int levels = 4) { setLevels(levels); }
    void setLevels(int levels) {
        levels_ = std::clamp(levels, 2, 255);
        rebuild();
    }
    [[nodiscard]] int levels() const noexcept { return levels_; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override {
        return AdjustmentKind::Posterize;
    }
    [[nodiscard]] std::string name() const override { return "Posterize"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<Posterize>(*this);
    }

private:
    void rebuild();

    int levels_ = 4;
    Lut1D lut_;
};

// Threshold: collapse each pixel to pure black or white by comparing its luminance
// against a level in [0,1] (default 0.5). Cross-channel; produces a 1-bit look.
class Threshold final : public Adjustment {
public:
    explicit Threshold(float level = 0.5f) : level_(clamp01(level)) {}
    void setLevel(float v) noexcept { level_ = clamp01(v); }
    [[nodiscard]] float level() const noexcept { return level_; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override {
        return AdjustmentKind::Threshold;
    }
    [[nodiscard]] std::string name() const override { return "Threshold"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<Threshold>(*this);
    }

private:
    float level_;
};

// Selective Color: per-range CMYK deltas across nine color/neutral ranges (reds,
// yellows, greens, cyans, blues, magentas, whites, neutrals, blacks). The six
// chromatic ranges are selected by hue (weighted by chroma); the three achromatic
// ranges by lightness band. In Relative mode a delta scales by the ink already
// present; in Absolute mode it is a flat shift. Each delta is in [-1,1]. The K
// (black) delta darkens (or, when negative, lightens) the result. Cross-channel.
class SelectiveColor final : public Adjustment {
public:
    enum Range {
        Reds = 0,
        Yellows,
        Greens,
        Cyans,
        Blues,
        Magentas,
        Whites,
        Neutrals,
        Blacks,
        kRangeCount
    };
    struct Cmyk {
        float c = 0.0f, m = 0.0f, y = 0.0f, k = 0.0f;
    };

    SelectiveColor() = default;

    void setRange(int range, float c, float m, float y, float k) noexcept {
        if (range < 0 || range >= kRangeCount) return;
        ranges_[static_cast<std::size_t>(range)] = {cl(c), cl(m), cl(y), cl(k)};
    }
    [[nodiscard]] Cmyk range(int r) const noexcept {
        return (r < 0 || r >= kRangeCount) ? Cmyk{} : ranges_[static_cast<std::size_t>(r)];
    }
    void setRelative(bool on) noexcept { relative_ = on; }
    [[nodiscard]] bool relative() const noexcept { return relative_; }

    [[nodiscard]] AdjustmentKind kind() const noexcept override {
        return AdjustmentKind::SelectiveColor;
    }
    [[nodiscard]] std::string name() const override { return "Selective Color"; }
    void apply(std::span<Rgbaf> tile) const override;
    [[nodiscard]] std::unique_ptr<Adjustment> clone() const override {
        return std::make_unique<SelectiveColor>(*this);
    }

private:
    static float cl(float v) noexcept { return std::clamp(v, -1.0f, 1.0f); }
    std::array<Cmyk, kRangeCount> ranges_{};
    bool relative_ = true;
};

}  // namespace pe
