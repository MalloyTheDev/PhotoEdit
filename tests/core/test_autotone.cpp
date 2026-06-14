#include "pe/core/AutoTone.hpp"
#include "pe/core/Histogram.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe_test.hpp"

#include <limits>
#include <vector>

using namespace pe;

namespace {
// Build a histogram from a list of gray levels (one pixel each).
Histogram grayHist(std::vector<int> levels) {
    Histogram h;
    for (int v : levels) {
        const auto u = static_cast<uint8_t>(v);
        h.accumulate(Rgba8{u, u, u, 255});
    }
    return h;
}
}  // namespace

PE_TEST(autotone_contrast_finds_endpoints) {
    // Gray values spanning 64..192: with no clipping the endpoints are 64 and 192.
    Histogram h = grayHist({64, 96, 128, 160, 192});
    AutoToneLevels lv = computeAutoTone(h, AutoToneMode::Contrast, 0.0);
    PE_CHECK_EQ(lv.blackPoint[0], 64);
    PE_CHECK_EQ(lv.whitePoint[0], 192);
    // Contrast applies the same endpoints to every channel.
    PE_CHECK_EQ(lv.blackPoint[2], 64);
    PE_CHECK_EQ(lv.whitePoint[2], 192);
}

PE_TEST(autotone_stretch_maps_black_and_white) {
    AutoToneLevels lv;
    lv.blackPoint = {64, 64, 64};
    lv.whitePoint = {192, 192, 192};
    std::vector<Rgbaf> px = {
        Rgbaf{64.0f / 255.0f, 64.0f / 255.0f, 64.0f / 255.0f, 1.0f},     // -> 0
        Rgbaf{192.0f / 255.0f, 192.0f / 255.0f, 192.0f / 255.0f, 1.0f},  // -> 1
        Rgbaf{128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f, 1.0f},  // -> ~0.5
    };
    applyAutoTone(px, lv);
    PE_CHECK_NEAR(px[0].r, 0.0f);
    PE_CHECK_NEAR(px[1].r, 1.0f);
    PE_CHECK_NEAR(px[2].r, 0.5f);
}

PE_TEST(autotone_clamps_outside_range) {
    AutoToneLevels lv;
    lv.blackPoint = {64, 64, 64};
    lv.whitePoint = {192, 192, 192};
    std::vector<Rgbaf> px = {
        Rgbaf{0.0f, 0.0f, 0.0f, 1.0f},  // below black -> clamps to 0
        Rgbaf{1.0f, 1.0f, 1.0f, 1.0f},  // above white -> clamps to 1
    };
    applyAutoTone(px, lv);
    PE_CHECK_NEAR(px[0].r, 0.0f);
    PE_CHECK_NEAR(px[1].r, 1.0f);
}

PE_TEST(autotone_degenerate_channel_is_identity) {
    // A flat histogram (all one level) gives black == white; the stretch must be a
    // safe no-op (no divide-by-zero), leaving pixels unchanged.
    Histogram h = grayHist({128, 128, 128, 128});
    AutoToneLevels lv = computeAutoTone(h, AutoToneMode::Levels, 0.0);
    PE_CHECK_EQ(lv.blackPoint[0], 128);
    PE_CHECK_EQ(lv.whitePoint[0], 128);
    std::vector<Rgbaf> px = {Rgbaf{0.3f, 0.6f, 0.9f, 1.0f}};
    applyAutoTone(px, lv);
    PE_CHECK_NEAR(px[0].r, 0.3f);
    PE_CHECK_NEAR(px[0].g, 0.6f);
    PE_CHECK_NEAR(px[0].b, 0.9f);
}

PE_TEST(autotone_levels_is_per_channel) {
    // Red spans 50..200, green/blue are flat at 128. Per-channel levels stretch only
    // red; Contrast mode (luma-based) would treat all channels alike.
    Histogram h;
    h.accumulate(Rgba8{50, 128, 128, 255});
    h.accumulate(Rgba8{200, 128, 128, 255});
    AutoToneLevels lv = computeAutoTone(h, AutoToneMode::Levels, 0.0);
    PE_CHECK_EQ(lv.blackPoint[0], 50);
    PE_CHECK_EQ(lv.whitePoint[0], 200);
    PE_CHECK_EQ(lv.blackPoint[1], 128);  // green flat
    PE_CHECK_EQ(lv.whitePoint[1], 128);
}

PE_TEST(autotone_skips_transparent_pixels) {
    AutoToneLevels lv;
    lv.blackPoint = {64, 64, 64};
    lv.whitePoint = {192, 192, 192};
    std::vector<Rgbaf> px = {Rgbaf{0.1f, 0.1f, 0.1f, 0.0f}};  // fully transparent
    applyAutoTone(px, lv);
    PE_CHECK_NEAR(px[0].r, 0.1f);  // unchanged
}

PE_TEST(autotone_nan_clip_falls_back_to_no_clip) {
    // A NaN clip fraction must not slip past the range clamp; it falls back to no
    // clipping (endpoints = lowest/highest populated levels), same as clip 0.
    Histogram h = grayHist({64, 128, 192});
    AutoToneLevels lv =
        computeAutoTone(h, AutoToneMode::Contrast, std::numeric_limits<double>::quiet_NaN());
    PE_CHECK_EQ(lv.blackPoint[0], 64);
    PE_CHECK_EQ(lv.whitePoint[0], 192);
}

PE_TEST(autotone_clip_fraction_trims_outliers) {
    // 100 pixels at 100, plus single outliers at 0 and 255. A 1% clip at each end
    // trims the lone outliers, pulling the endpoints in to 100.
    std::vector<int> levels;
    levels.push_back(0);
    for (int i = 0; i < 100; ++i) levels.push_back(100);
    levels.push_back(255);
    Histogram h = grayHist(levels);
    AutoToneLevels noClip = computeAutoTone(h, AutoToneMode::Contrast, 0.0);
    PE_CHECK_EQ(noClip.blackPoint[0], 0);
    PE_CHECK_EQ(noClip.whitePoint[0], 255);
    AutoToneLevels clipped = computeAutoTone(h, AutoToneMode::Contrast, 0.02);
    PE_CHECK_EQ(clipped.blackPoint[0], 100);
    PE_CHECK_EQ(clipped.whitePoint[0], 100);
}
