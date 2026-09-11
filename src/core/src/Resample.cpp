#include "pe/core/Resample.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pe {

namespace {

// Catmull-Rom (Keys a = -1/2), argument already in kernel space (source distance / h).
[[nodiscard]] double catmull(double t) noexcept {
    t = std::fabs(t);
    if (t <= 1.0) return ((1.5 * t - 2.5) * t) * t + 1.0;
    if (t < 2.0) return (((-0.5 * t + 2.5) * t) - 4.0) * t + 2.0;
    return 0.0;
}

[[nodiscard]] int clampIndex(int i, int len) noexcept {
    return i < 0 ? 0 : (i >= len ? len - 1 : i);
}

}  // namespace

ResampleAxis buildResampleAxis(int srcLen, int dstLen) {
    ResampleAxis ax;
    if (srcLen <= 0 || dstLen <= 0) return ax;

    // scale = dst/src. On upscale (scale > 1) the kernel is unstretched (h = 1) and stays
    // interpolating; on downscale it is stretched by 1/scale so its cutoff drops to the output
    // Nyquist, which is the anti-alias low-pass.
    const double scale = static_cast<double>(dstLen) / static_cast<double>(srcLen);
    const double h = scale < 1.0 ? 1.0 / scale : 1.0;
    const double radius = 2.0 * h;  // Catmull-Rom radius is 2, in kernel units, stretched by h

    // One fixed tap count for the axis, over-provisioned by a tap so a sample whose footprint
    // shifts with sub-pixel phase still has room; taps that fall outside the kernel's [-2h, 2h]
    // contribute a weight of exactly zero, so the slack costs nothing but a multiply.
    const int support = static_cast<int>(std::ceil(2.0 * radius)) + 1;
    ax.support = support;
    ax.first.resize(static_cast<std::size_t>(dstLen));
    ax.weights.assign(static_cast<std::size_t>(dstLen) * static_cast<std::size_t>(support), 0.0f);

    for (int o = 0; o < dstLen; ++o) {
        // Output pixel centre mapped to source space, the standard half-pixel convention: an
        // integer scale then lands `center` exactly on a source pixel, where Catmull-Rom is a
        // delta and the resample is the identity.
        const double center = (static_cast<double>(o) + 0.5) / scale - 0.5;
        const int first = static_cast<int>(std::ceil(center - radius));
        ax.first[static_cast<std::size_t>(o)] = first;

        double sum = 0.0;
        const std::size_t base = static_cast<std::size_t>(o) * static_cast<std::size_t>(support);
        for (int t = 0; t < support; ++t) {
            const double d = (center - static_cast<double>(first + t)) / h;
            const double w = catmull(d);
            ax.weights[base + static_cast<std::size_t>(t)] = static_cast<float>(w);
            sum += w;
        }
        if (sum != 0.0) {
            const float inv = static_cast<float>(1.0 / sum);
            for (int t = 0; t < support; ++t) ax.weights[base + static_cast<std::size_t>(t)] *= inv;
        }
    }
    return ax;
}

std::vector<Rgbaf> resampleImage(const std::vector<Rgbaf>& src, int srcW, int srcH, int dstW,
                                 int dstH) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return {};
    if (src.size() != static_cast<std::size_t>(srcW) * static_cast<std::size_t>(srcH)) return {};

    // Premultiply once; every tap and both passes work in premultiplied space.
    std::vector<Rgbaf> pm(src.size());
    for (std::size_t i = 0; i < src.size(); ++i) pm[i] = premultiply(src[i]);

    const ResampleAxis ax = buildResampleAxis(srcW, dstW);
    const ResampleAxis ay = buildResampleAxis(srcH, dstH);

    // Horizontal pass into a dstW x srcH intermediate, still premultiplied. Contiguous, because
    // this is the bounded reference resampler (text rasters, selection masks, and the oracle the
    // tile-streamed layer path is tested against); the layer path bands this per tile instead.
    std::vector<Rgbaf> tmp(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(srcH));
    for (int y = 0; y < srcH; ++y) {
        const std::size_t srcRow = static_cast<std::size_t>(y) * static_cast<std::size_t>(srcW);
        const std::size_t tmpRow = static_cast<std::size_t>(y) * static_cast<std::size_t>(dstW);
        for (int x = 0; x < dstW; ++x) {
            const int first = ax.first[static_cast<std::size_t>(x)];
            const std::size_t wb =
                static_cast<std::size_t>(x) * static_cast<std::size_t>(ax.support);
            Rgbaf acc{0.0f, 0.0f, 0.0f, 0.0f};
            for (int t = 0; t < ax.support; ++t) {
                const float w = ax.weights[wb + static_cast<std::size_t>(t)];
                if (w == 0.0f) continue;
                const Rgbaf& p = pm[srcRow + static_cast<std::size_t>(clampIndex(first + t, srcW))];
                acc.r += p.r * w;
                acc.g += p.g * w;
                acc.b += p.b * w;
                acc.a += p.a * w;
            }
            tmp[tmpRow + static_cast<std::size_t>(x)] = acc;
        }
    }

    // Vertical pass into the output, then unpremultiply.
    std::vector<Rgbaf> out(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH));
    for (int y = 0; y < dstH; ++y) {
        const int first = ay.first[static_cast<std::size_t>(y)];
        const std::size_t wb = static_cast<std::size_t>(y) * static_cast<std::size_t>(ay.support);
        const std::size_t outRow = static_cast<std::size_t>(y) * static_cast<std::size_t>(dstW);
        for (int x = 0; x < dstW; ++x) {
            Rgbaf acc{0.0f, 0.0f, 0.0f, 0.0f};
            for (int t = 0; t < ay.support; ++t) {
                const float w = ay.weights[wb + static_cast<std::size_t>(t)];
                if (w == 0.0f) continue;
                const Rgbaf& p = tmp[static_cast<std::size_t>(clampIndex(first + t, srcH)) *
                                         static_cast<std::size_t>(dstW) +
                                     static_cast<std::size_t>(x)];
                acc.r += p.r * w;
                acc.g += p.g * w;
                acc.b += p.b * w;
                acc.a += p.a * w;
            }
            // Unpremultiply. Divide colour by the raw premultiplied alpha (which recovers the
            // straight colour even when ringing pushed it slightly past 1), then clamp the
            // stored alpha to [0,1] since coverage cannot exceed 1 or fall below 0. RGB is left
            // unclamped for HDR; the 8/16-bit stores clamp on write, the float store does not.
            if (acc.a <= 0.0f) {
                out[outRow + static_cast<std::size_t>(x)] = Rgbaf{0.0f, 0.0f, 0.0f, 0.0f};
            } else {
                const float inv = 1.0f / acc.a;
                const float oa = acc.a > 1.0f ? 1.0f : acc.a;
                out[outRow + static_cast<std::size_t>(x)] =
                    Rgbaf{acc.r * inv, acc.g * inv, acc.b * inv, oa};
            }
        }
    }
    return out;
}

std::vector<std::uint8_t> resampleCoverage(const std::vector<std::uint8_t>& src, int srcW, int srcH,
                                           int dstW, int dstH) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return {};
    if (src.size() != static_cast<std::size_t>(srcW) * static_cast<std::size_t>(srcH)) return {};

    const ResampleAxis ax = buildResampleAxis(srcW, dstW);
    const ResampleAxis ay = buildResampleAxis(srcH, dstH);

    std::vector<float> tmp(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(srcH));
    for (int y = 0; y < srcH; ++y) {
        const std::size_t srcRow = static_cast<std::size_t>(y) * static_cast<std::size_t>(srcW);
        const std::size_t tmpRow = static_cast<std::size_t>(y) * static_cast<std::size_t>(dstW);
        for (int x = 0; x < dstW; ++x) {
            const int first = ax.first[static_cast<std::size_t>(x)];
            const std::size_t wb =
                static_cast<std::size_t>(x) * static_cast<std::size_t>(ax.support);
            float acc = 0.0f;
            for (int t = 0; t < ax.support; ++t) {
                const float w = ax.weights[wb + static_cast<std::size_t>(t)];
                if (w == 0.0f) continue;
                acc += static_cast<float>(
                           src[srcRow + static_cast<std::size_t>(clampIndex(first + t, srcW))]) *
                       w;
            }
            tmp[tmpRow + static_cast<std::size_t>(x)] = acc;
        }
    }

    std::vector<std::uint8_t> out(static_cast<std::size_t>(dstW) * static_cast<std::size_t>(dstH));
    for (int y = 0; y < dstH; ++y) {
        const int first = ay.first[static_cast<std::size_t>(y)];
        const std::size_t wb = static_cast<std::size_t>(y) * static_cast<std::size_t>(ay.support);
        const std::size_t outRow = static_cast<std::size_t>(y) * static_cast<std::size_t>(dstW);
        for (int x = 0; x < dstW; ++x) {
            float acc = 0.0f;
            for (int t = 0; t < ay.support; ++t) {
                const float w = ay.weights[wb + static_cast<std::size_t>(t)];
                if (w == 0.0f) continue;
                acc += tmp[static_cast<std::size_t>(clampIndex(first + t, srcH)) *
                               static_cast<std::size_t>(dstW) +
                           static_cast<std::size_t>(x)] *
                       w;
            }
            const long v = std::lround(acc);
            out[outRow + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    return out;
}

}  // namespace pe
