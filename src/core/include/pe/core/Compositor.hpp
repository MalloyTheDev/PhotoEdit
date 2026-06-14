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

// Upper bound on the pixel count compositeToImage will rasterize in one call
// (~64 MP). The whole-image path eagerly allocates width*height*4 bytes, so this
// caps memory and guards against overflow/DoS on very large canvases. Documents
// larger than this must be displayed via the tile-based viewport (M2), which
// composites only the visible/dirty tiles. compositeToImage returns an empty
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

}  // namespace pe
