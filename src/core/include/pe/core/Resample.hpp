#pragma once

#include "pe/core/Color.hpp"

#include <cstdint>
#include <vector>

namespace pe {

// Separable Catmull-Rom image resampling: the arithmetic behind Image Size, and the future
// replacement for the aliasing bilinear in Free Transform's downscale.
//
// Catmull-Rom (Keys a = -1/2) is interpolating: at an integer scale it reproduces the source
// exactly, so nudging a dimension a few pixels does not soften an image that did not change
// size. On DOWNSCALE the kernel is stretched by h = 1/scale per axis, which moves its cutoff
// from source Nyquist down to output Nyquist: that stretch IS the low-pass, so there is no
// separate box/mipmap path and no seam at 0.5x. Bilinear, by contrast, reads too few source
// pixels below 0.5x and drops most of the image.
//
// Sampling is clamp-to-edge (a tap past the border reads the edge pixel), because Image Size
// resamples a canvas-filling image whose edge is real content, not a layer that ends in
// transparency. Colour is resampled PREMULTIPLIED so a transparent pixel's arbitrary RGB cannot
// bleed into a visible neighbour, and the result's alpha is clamped to [0,1] because the
// kernel's negative lobes overshoot: an un-clamped negative alpha becomes an invisible hole
// once unpremultiplied.

// One output axis's weights: for each output sample, the source index of its first tap and a
// run of `support` weights. Shared so the tile-streamed layer resampler and this contiguous one
// use one definition of the kernel and cannot drift. Weights are normalised to sum to 1 (a
// stretched Catmull-Rom does not partition unity at arbitrary phase, so this is not free).
struct ResampleAxis {
    int support = 0;             // taps per output sample
    std::vector<int> first;      // size dstLen: source index of output o's first tap (may be < 0)
    std::vector<float> weights;  // size dstLen * support, row-major by output index

    [[nodiscard]] bool empty() const noexcept { return support == 0 || first.empty(); }
};

// Build the weight table mapping `srcLen` source samples to `dstLen` output samples along one
// axis. Empty for a non-positive length. A tap index outside [0, srcLen) is the caller's to
// clamp (edge replicate); the weights already account for the stretch.
[[nodiscard]] ResampleAxis buildResampleAxis(int srcLen, int dstLen);

// Resample a contiguous, straight-alpha, row-major RGBA float image from srcW x srcH to
// dstW x dstH. Alpha comes back in [0,1]; RGB is left unclamped so a 32-bit-float layer's
// out-of-range colour survives (an 8/16-bit store clamps it on write). Empty for a non-positive
// size or a `src` whose length is not srcW*srcH.
[[nodiscard]] std::vector<Rgbaf> resampleImage(const std::vector<Rgbaf>& src, int srcW, int srcH,
                                               int dstW, int dstH);

// Resample a single-channel [0,255] coverage plane (a mask, or a selection's coverage). No
// premultiply, since coverage has no alpha to bleed; the result is clamped to [0,255] because
// the kernel overshoots. Empty for a non-positive size or a length mismatch.
[[nodiscard]] std::vector<std::uint8_t> resampleCoverage(const std::vector<std::uint8_t>& src,
                                                         int srcW, int srcH, int dstW, int dstH);

}  // namespace pe
