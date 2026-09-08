#pragma once

#include "pe/core/Color.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/Tile.hpp"

#include <cstdint>
#include <memory>
#include <span>

namespace pe {

// Maximum group-nesting depth the compositor descends before treating deeper
// groups as transparent. Bounds recursion so a pathologically nested tree cannot
// overflow the stack (security hardening). 64 is far beyond any real document.
inline constexpr int kMaxCompositeDepth = 64;

// Upper bound on the pixel count the whole-image flatten will rasterize in one
// call (~64 MP), shared by compositeToImage, compositeToImage16, and
// compositeToImageF. The path eagerly allocates the full buffer — width*height
// times 4 bytes at 8-bit, 8 bytes at 16-bit, 16 bytes for the float path — so this
// caps memory and guards against overflow/DoS on very large canvases. Documents
// larger than this must be displayed via the tile-based viewport (M2), which
// composites only visible/dirty tiles. All three entry points return an empty
// buffer if the canvas exceeds this budget.
inline constexpr int64_t kMaxCompositeImagePixels = 64'000'000;

// Composite a layer stack (bottom-to-top) for a single tile, blending each
// visible layer's straight-alpha contribution onto `acc` (size == kTilePixels,
// tile-local row-major; the caller pre-fills it, normally transparent). Honors
// visibility, opacity, and blend mode; groups recurse with depth tracking.
void compositeStack(std::span<const std::unique_ptr<Layer>> stack, TileCoord coord,
                    std::span<Rgbaf> acc, int depth = 0);

// Composite a layer stack over `canvas` (document-space rect) into an 8-bit
// straight-alpha image whose origin maps to canvas.topLeft. This is the headless
// entry point the golden-image tests use and the canvas viewport will drive.
[[nodiscard]] PixelBuffer compositeToImage(std::span<const std::unique_ptr<Layer>> stack,
                                           Rect canvas);

// The most source tiles a scaled composite will read before giving up. A scaled composite
// has no AREA cap, deliberately: bounding it by area would defeat the point, which is to
// preview something too large to flatten. What it is bounded by is the work, and the work is
// tiles. Matches the renderer's own scaled path.
inline constexpr std::int64_t kMaxScaledSourceTiles = 16384;

// Composite `stack` over `region` and box-average it down by an integer `divisor`, producing
// an image of ceil(region.width / divisor) x ceil(region.height / divisor).
//
// The full-resolution region is NEVER materialized: tiles are composited one at a time and
// accumulated into the output bins, so peak memory is one tile plus the output rather than
// the whole region. That is what lets a caller preview a layer far larger than
// kMaxCompositeImagePixels, which compositeToImage refuses outright.
//
// Averaging is done PREMULTIPLIED and then un-premultiplied, because averaging straight alpha
// across transparent pixels biases the colour toward black. Accumulators are double: a bin
// sums up to divisor^2 samples, which passes float32's exact-integer limit at large
// downscales and would silently collapse the average toward zero.
//
// Returns an empty buffer for an empty or unrepresentable region, a divisor below 1, or a
// region spanning more than `maxSourceTiles`.
// Diagnostics: source tiles composited by compositeToImageScaled since the process started.
//
// The output is a fixed small size whatever the input, so a preview that reads the whole
// canvas and one that reads only the layer's content produce IDENTICAL pixels. The difference
// is entirely in the work, and the layers panel exists to make that difference: a thumbnail
// used to composite the full canvas at full resolution for a 26x26 icon. Same idiom as
// NativeFormat's gatherTileLookupCount.
[[nodiscard]] std::uint64_t scaledCompositeTileCount() noexcept;

[[nodiscard]] PixelBuffer compositeToImageScaled(
    std::span<const std::unique_ptr<Layer>> stack, Rect region, int divisor,
    std::int64_t maxSourceTiles = kMaxScaledSourceTiles);

// Like compositeToImage, but preserves the full 32-bit-float composite (no 8-bit
// quantization) — the high-bit-depth flatten/export path (docs/systems/15). The
// same megapixel budget applies; returns an empty buffer if the canvas exceeds it.
// Unlike the 8-bit path (which clamps and sinks NaN via toRgba8), the float output
// is passed through verbatim: it intentionally retains out-of-range/HDR values
// (> 1.0) and would surface any NaN from a degenerate source, so a downstream
// float consumer (16/32-bit export) owns any sanitization it needs.
[[nodiscard]] PixelBufferF compositeToImageF(std::span<const std::unique_ptr<Layer>> stack,
                                             Rect canvas);

// Like compositeToImage, but quantizes the float composite to 16-bit (round, clamp,
// and NaN-sink via toRgba16) — the 16-bit flatten/export path (docs/systems/15).
// The same megapixel budget applies; returns an empty buffer if the canvas exceeds
// it. Unlike the float path, this clamps to [0,1] and is NaN-safe.
[[nodiscard]] PixelBuffer16 compositeToImage16(std::span<const std::unique_ptr<Layer>> stack,
                                               Rect canvas);

}  // namespace pe
