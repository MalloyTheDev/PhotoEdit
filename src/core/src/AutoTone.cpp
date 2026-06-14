#include "pe/core/AutoTone.hpp"

namespace pe {

namespace {
// Color channels only; a fully transparent pixel has no color to stretch.
inline bool opaqueEnough(const Rgbaf& p) noexcept {
    return p.a > 0.0f;
}

uint64_t totalOf(const Histogram::Bins& bins) noexcept {
    uint64_t t = 0;
    for (const uint64_t c : bins) t += c;
    return t;
}

// Lowest level whose cumulative-from-black count exceeds clipCount (the darkest
// clipCount pixels are discarded). With clipCount == 0 this is the lowest populated
// level. An empty channel (no level ever exceeds 0) returns the top level.
int lowEndpoint(const Histogram::Bins& bins, double clipCount) noexcept {
    double cum = 0.0;
    for (int i = 0; i < Histogram::kBins; ++i) {
        cum += static_cast<double>(bins[static_cast<std::size_t>(i)]);
        if (cum > clipCount) return i;
    }
    return Histogram::kBins - 1;
}

// Highest level whose cumulative-from-white count exceeds clipCount. An empty
// channel returns the bottom level, so (white <= black) -> a safe identity stretch.
int highEndpoint(const Histogram::Bins& bins, double clipCount) noexcept {
    double cum = 0.0;
    for (int i = Histogram::kBins - 1; i >= 0; --i) {
        cum += static_cast<double>(bins[static_cast<std::size_t>(i)]);
        if (cum > clipCount) return i;
    }
    return 0;
}

void endpointsFor(const Histogram::Bins& bins, double clip, int& black, int& white) noexcept {
    const double clipCount = clip * static_cast<double>(totalOf(bins));
    black = lowEndpoint(bins, clipCount);
    white = highEndpoint(bins, clipCount);
}
}  // namespace

AutoToneLevels computeAutoTone(const Histogram& hist, AutoToneMode mode, double clipFraction) {
    const double clip = clipFraction < 0.0 ? 0.0 : (clipFraction > 0.49 ? 0.49 : clipFraction);

    AutoToneLevels out;
    if (mode == AutoToneMode::Contrast) {
        // One black/white pair from the luma histogram, shared by all channels.
        int black = 0, white = 255;
        endpointsFor(hist.luma, clip, black, white);
        for (int c = 0; c < 3; ++c) {
            out.blackPoint[static_cast<std::size_t>(c)] = black;
            out.whitePoint[static_cast<std::size_t>(c)] = white;
        }
    } else {
        // Per-channel endpoints from each channel's own histogram.
        const Histogram::Bins* chans[3] = {&hist.red, &hist.green, &hist.blue};
        for (int c = 0; c < 3; ++c) {
            endpointsFor(*chans[c], clip, out.blackPoint[static_cast<std::size_t>(c)],
                         out.whitePoint[static_cast<std::size_t>(c)]);
        }
    }
    return out;
}

void applyAutoTone(std::span<Rgbaf> tile, const AutoToneLevels& levels) {
    // Precompute per-channel black (in [0,1]) and inverse span; a degenerate channel
    // (white <= black) maps to scale 0, which we treat as identity below.
    float black[3];
    float invSpan[3];
    bool identity[3];
    for (int c = 0; c < 3; ++c) {
        const int b = levels.blackPoint[static_cast<std::size_t>(c)];
        const int w = levels.whitePoint[static_cast<std::size_t>(c)];
        identity[c] = w <= b;
        black[c] = static_cast<float>(b) / 255.0f;
        invSpan[c] = identity[c] ? 0.0f : 255.0f / static_cast<float>(w - b);
    }
    const auto map = [&](float v, int c) {
        if (identity[c]) return clamp01(v);
        return clamp01((v - black[c]) * invSpan[c]);
    };
    for (Rgbaf& p : tile) {
        if (!opaqueEnough(p)) continue;
        p.r = map(p.r, 0);
        p.g = map(p.g, 1);
        p.b = map(p.b, 2);
    }
}

}  // namespace pe
