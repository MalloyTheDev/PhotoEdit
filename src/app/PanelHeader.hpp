#pragma once

#include <QString>

class QWidget;

namespace pe::app {

// Build a compact dock title bar: a small dim glyph + a small-caps, letter-spaced
// label on the themed header strip with a hairline bottom border. Install via
// QDockWidget::setTitleBarWidget to replace the default OS-style title.
[[nodiscard]] QWidget* makePanelHeader(const QString& title, const QString& iconName,
                                       QWidget* parent);

}  // namespace pe::app
