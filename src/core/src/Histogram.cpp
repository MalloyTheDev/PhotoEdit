#include "pe/core/Histogram.hpp"

#include "pe/core/PixelBuffer.hpp"

#include <cmath>

namespace pe {

namespace {
// Rec.601 luma on encoded 8-bit values, rounded to a [0,255] bin. Matches the
// luminance() used by the adjustment operators (GradientMap/Threshold).
inline int luma8(Rgba8 p) noexcept {
    const float l = 0.299f * static_cast<float>(p.r) + 0.587f * static_cast<float>(p.g) +
                    0.114f * static_cast<float>(p.b);
    const int v = static_cast<int>(l + 0.5f);
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}
}  // namespace

void Histogram::accumulate(Rgba8 p) noexcept {
    ++red[p.r];
    ++green[p.g];
    ++blue[p.b];
    ++alpha[p.a];
    ++luma[static_cast<std::size_t>(luma8(p))];
    ++count;
}

Histogram computeHistogram(std::span<const Rgba8> pixels) {
    Histogram h;
    for (const Rgba8 p : pixels) h.accumulate(p);
    return h;
}

Histogram computeHistogram(const PixelBuffer& img) {
    if (img.isEmpty()) return Histogram{};
    const std::size_t n =
        static_cast<std::size_t>(img.width()) * static_cast<std::size_t>(img.height());
    return computeHistogram(std::span<const Rgba8>(img.data(), n));
}

ChannelStats channelStats(const Histogram::Bins& bins) noexcept {
    ChannelStats s;
    uint64_t total = 0;
    double sum = 0.0;
    bool sawAny = false;
    for (int i = 0; i < Histogram::kBins; ++i) {
        const uint64_t c = bins[static_cast<std::size_t>(i)];
        if (c == 0) continue;
        total += c;
        sum += static_cast<double>(c) * static_cast<double>(i);
        if (!sawAny) {
            s.minLevel = i;
            sawAny = true;
        }
        s.maxLevel = i;
        if (c > s.mode) {
            s.mode = c;
            s.modeLevel = i;
        }
    }
    s.count = total;
    if (total == 0) return s;  // empty channel: zeros except defaults

    s.mean = sum / static_cast<double>(total);
    // Variance in a second pass over the (cheap, fixed-size) bins.
    double var = 0.0;
    for (int i = 0; i < Histogram::kBins; ++i) {
        const uint64_t c = bins[static_cast<std::size_t>(i)];
        if (c == 0) continue;
        const double d = static_cast<double>(i) - s.mean;
        var += static_cast<double>(c) * d * d;
    }
    s.stdDev = std::sqrt(var / static_cast<double>(total));
    s.median = percentileLevel(bins, 0.5);
    return s;
}

int percentileLevel(const Histogram::Bins& bins, double percentile) noexcept {
    uint64_t total = 0;
    for (const uint64_t c : bins) total += c;
    if (total == 0) return 0;

    const double p = percentile < 0.0 ? 0.0 : (percentile > 1.0 ? 1.0 : percentile);
    // Smallest level whose cumulative count reaches the target fraction of pixels.
    const double target = p * static_cast<double>(total);
    uint64_t cumulative = 0;
    for (int i = 0; i < Histogram::kBins; ++i) {
        cumulative += bins[static_cast<std::size_t>(i)];
        if (static_cast<double>(cumulative) >= target) return i;
    }
    return Histogram::kBins - 1;
}

}  // namespace pe
