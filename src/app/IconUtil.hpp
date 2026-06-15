#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

namespace pe::app {

// Render a bundled Lucide glyph (icons.qrc) to a crisp HiDPI pixmap, re-tinted to
// `color` (the SVGs ship a neutral light stroke that we recolor per use — light for
// the tool strip, dim for panel headers). `logical` is the logical (pre-DPR) size.
[[nodiscard]] QPixmap renderIcon(const QString& name, const QColor& color, int logical);

// Same, as a QIcon for buttons/actions.
[[nodiscard]] QIcon renderIconAsIcon(const QString& name, const QColor& color, int logical);

}  // namespace pe::app
