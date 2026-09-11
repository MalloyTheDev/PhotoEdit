#pragma once

#include "pe/core/Document.hpp"  // kMaxCanvasDimension

#include <QSpinBox>

namespace pe::app {

// A pixel-dimension spin box, set up the one way every size field in the app wants it: whole
// pixels from 1 to the engine's own per-side limit, with a " px" suffix so the unit is on the
// control rather than in a separate label. The New and Canvas Size dialogs both need exactly
// this; sharing it means their two fields cannot drift apart on the range that actually bounds
// a valid canvas. The caller sets the object name and the initial value it wants.
[[nodiscard]] inline QSpinBox* makeDimensionSpinBox(QWidget* parent, int value) {
    auto* box = new QSpinBox(parent);
    box->setRange(1, pe::kMaxCanvasDimension);
    box->setValue(value);
    box->setSuffix(QStringLiteral(" px"));
    return box;
}

}  // namespace pe::app
