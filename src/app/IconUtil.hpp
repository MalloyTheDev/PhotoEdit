#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace pe::app {

// Register the bundled icon resources.
//
// pe_app is a static library, so the linker discards the rcc-generated resource
// initializer unless a linked translation unit references it: without this call
// every `:/icons/...` lookup fails and the tool strip renders as blank squares.
// Call once from main() before constructing any UI. Repeat calls are harmless.
void initIconResources();

// Render a bundled Lucide glyph (icons.qrc) to a crisp HiDPI pixmap, re-tinted to
// `color` (the SVGs ship a neutral light stroke that we recolor per use — light for
// the tool strip, dim for panel headers). `logical` is the logical (pre-DPR) size.
// `dpr` is the device pixel ratio to rasterize for; pass 0 to use the display's
// actual ratio. This was previously hard-coded to 2.0, so the glyphs were rendered
// at the wrong resolution on every display that is not exactly 2x.
[[nodiscard]] QPixmap renderIcon(const QString& name, const QColor& color, int logical,
                                 qreal dpr = 0.0);

// Same, as a QIcon carrying 1x, 2x and 3x rasterizations so Qt can pick the right
// one per screen, including after a window is dragged to a display with a different
// scale factor.
[[nodiscard]] QIcon renderIconAsIcon(const QString& name, const QColor& color, int logical);

// The colour bundled glyphs should be tinted with for the active theme. Icons are
// foreground marks, so they follow the theme's text colour; the tint used to be a
// hard-coded constant, which left them unchanged when the theme changed.
[[nodiscard]] QColor themeIconColor();

}  // namespace pe::app
