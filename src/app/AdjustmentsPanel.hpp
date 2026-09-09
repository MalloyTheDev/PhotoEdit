#pragma once

#include "pe/core/Adjustment.hpp"

#include <QImage>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>
#include <memory>

class QEvent;
class QListWidget;
class QListWidgetItem;
class QObject;

namespace pe::app {

// The Adjustments dock: a list of one-click presets, each of which adds one configured,
// non-destructive adjustment layer.
//
// This dock used to hold a centred label reading "Adjustments" and nothing else, which is
// what the user was describing when they said the panels on the right were empty. Layer >
// New Adjustment Layer already existed, but every entry there creates the adjustment at its
// IDENTITY settings: a fresh Levels layer changes nothing at all until its dialog is opened
// and numbers are typed in. So the honest gap was never "there is no way to add one", it was
// "there is no way to add one that DOES anything". A preset is that: a name for a look, and
// the settings that produce it.
//
// Each row's swatch is not a drawing of what the preset might do. It is referenceStrip() with
// that preset's own pe::Adjustment applied to it by the engine, so the row cannot claim an
// effect the layer will not produce, and a change to an adjustment's maths shows up here.
//
// Like ColorPanel and SwatchesPanel this knows nothing about the document: it announces the
// chosen preset and MainWindow, which owns the document and the undo stack, builds the layer.
class AdjustmentsPanel : public QWidget {
    Q_OBJECT

public:
    struct Preset {
        QString group;        // the heading it sits under
        QString name;         // what the layer will be called
        QString type;         // the adjustment it is built from, e.g. "Levels"
        QString description;  // what it does to an image; the row's tooltip
        std::function<std::unique_ptr<pe::Adjustment>()> make;
    };

    explicit AdjustmentsPanel(QWidget* parent = nullptr);

    [[nodiscard]] int presetCount() const noexcept { return static_cast<int>(presets_.size()); }
    // The preset at `index`. An out-of-range index yields a default-constructed entry whose
    // make is empty, rather than reading off the end.
    [[nodiscard]] const Preset& preset(int index) const;
    // A fresh adjustment configured as `index` describes; null for an out-of-range index.
    // Fresh every call: two layers made from one preset must not share state.
    [[nodiscard]] std::unique_ptr<pe::Adjustment> makeAdjustment(int index) const;

    // The strip every preview is computed from: a black-to-white tone ramp over a hue sweep,
    // so tonal presets show in the top half and colour presets in the bottom. Public because
    // asserting a preview differs from it is the only way to catch a preset that was
    // configured to do nothing, which is exactly the failure this panel exists to fix.
    [[nodiscard]] static QImage referenceStrip();
    // referenceStrip() with preset `index` applied by the engine. Null if out of range.
    [[nodiscard]] QImage preview(int index) const;

    // What activating a row does. One path for the mouse, the keyboard and the tests, so a
    // fix to any of them cannot miss the others. Out-of-range indices (a heading row) do
    // nothing rather than emitting.
    void activate(int index);

signals:
    void presetChosen(int index);

protected:
    // Return/Enter/Space on the list activates the current row. QListWidget::itemActivated
    // would be the obvious route, but on some styles it also fires for a single click, which
    // would add two layers per click.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildPresets();
    void buildRows();
    // The preset a list row carries, or -1 for a heading row.
    [[nodiscard]] int presetForRow(int row) const;

    QVector<Preset> presets_;
    QListWidget* list_ = nullptr;
};

}  // namespace pe::app
