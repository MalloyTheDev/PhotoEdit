#pragma once

#include "pe/core/Color.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace pe {

class PixelBuffer;

// 256-bin histograms over an 8-bit RGBA raster: per-channel R/G/B/A plus a
// luminance histogram (Rec.601 luma, the same model the adjustments use). Drives
// the Histogram/Info panels and statistics. Bins are uint64 so a full-resolution
// document (hundreds of megapixels) cannot overflow a single bin. See
// docs/systems/05-adjustment-layers.md and the M5 roadmap (Histogram/Info panels).
struct Histogram {
    static constexpr int kBins = 256;
    using Bins = std::array<uint64_t, kBins>;

    Bins red{};
    Bins green{};
    Bins blue{};
    Bins alpha{};
    Bins luma{};
    uint64_t count = 0;  // total pixels tallied

    // Add one 8-bit pixel to every channel histogram.
    void accumulate(Rgba8 p) noexcept;
};

// Summary statistics computed from one channel's 256 bins.
struct ChannelStats {
    double mean = 0.0;    // average level [0,255]
    double stdDev = 0.0;  // standard deviation of levels
    int median = 0;       // level at the 50th percentile
    int minLevel = 0;     // lowest level with any pixels (0 if empty)
    int maxLevel = 0;     // highest level with any pixels (0 if empty)
    uint64_t count = 0;   // pixels tallied in this channel
    uint64_t mode = 0;    // population of the most-populated level
    int modeLevel = 0;    // the most-populated level
};

// Tally R/G/B/A/luma over the whole buffer. Every pixel is counted in every
// channel (matching Photoshop's "RGB" histogram, which is alpha-independent).
[[nodiscard]] Histogram computeHistogram(const PixelBuffer& img);

// Tally over a contiguous span of pixels (used by tile-streaming callers).
[[nodiscard]] Histogram computeHistogram(std::span<const Rgba8> pixels);

// Statistics for one channel's bins. The level at a given cumulative percentile
// in [0,1]; percentileLevel(bins, 0.5) is the median.
[[nodiscard]] ChannelStats channelStats(const Histogram::Bins& bins) noexcept;
[[nodiscard]] int percentileLevel(const Histogram::Bins& bins, double percentile) noexcept;

}  // namespace pe
