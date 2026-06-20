#pragma once

#include "pe/core/PixelBuffer.hpp"
#include "pe/core/TextLayer.hpp"  // pe::TextModel

class QColor;
class QFont;
class QString;

namespace pe::app {

// Rasterize a single line of `text` in `font` and `color` to a straight-alpha RGBA8 buffer that
// tightly bounds the glyphs (baseline laid out, with a small antialiasing margin). Returns an
// empty buffer for empty text or if the rendered size would exceed a sane area cap. Lives in the
// app layer because it uses Qt's font/painter stack; the buffer becomes a pe::TextLayer's cached
// raster (via rasterizeText), which the headless engine composites. The ink offset is baked into
// the buffer here (glyphs laid out at the margin), so the layer places the raster's (0,0) at the
// click point with no separate offset.
[[nodiscard]] pe::PixelBuffer renderText(const QString& text, const QFont& font,
                                         const QColor& color);

// Rasterize a TextModel into the cached raster a pe::TextLayer composites: builds the QFont from
// the model (family / pixelSize / bold / italic) and the QColor from model.color, then delegates to
// renderText. Empty buffer on empty/over-cap text. This is the single producer the engine's
// TextLayer raster contract (pe::kMaxTextRasterDim/Pixels) is aligned to.
[[nodiscard]] pe::PixelBuffer rasterizeText(const pe::TextModel& model);

}  // namespace pe::app
