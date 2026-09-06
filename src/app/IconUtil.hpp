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
[[nodiscard]] QPixmap renderIcon(const QString& name, const QColor& color, int logical);

// Same, as a QIcon for buttons/actions.
[[nodiscard]] QIcon renderIconAsIcon(const QString& name, const QColor& color, int logical);

}  // namespace pe::app
