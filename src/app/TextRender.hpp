#pragma once

#include "pe/core/PixelBuffer.hpp"

class QColor;
class QFont;
class QString;

namespace pe::app {

// Rasterize a single line of `text` in `font` and `color` to a straight-alpha RGBA8 buffer that
// tightly bounds the glyphs (baseline laid out, with a small antialiasing margin). Returns an
// empty buffer for empty text or if the rendered size would exceed a sane area cap. Lives in the
// app layer because it uses Qt's font/painter stack; the headless engine then composites the
// result onto a layer via pe::stampBuffer.
[[nodiscard]] pe::PixelBuffer renderText(const QString& text, const QFont& font,
                                         const QColor& color);

}  // namespace pe::app
