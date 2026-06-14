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
// call (~64 MP), shared by compositeToImage and compositeToImageF. The path eagerly
// allocates the full buffer — width*height*4 bytes at 8-bit, *16 bytes for the
// float path — so this caps memory and guards against overflow/DoS on very large
// canvases. Documents larger than this must be displayed via the tile-based
// viewport (M2), which composites only visible/dirty tiles. Both entry points
// return an empty buffer if the canvas exceeds this budget.
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
