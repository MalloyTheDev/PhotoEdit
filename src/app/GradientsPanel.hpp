#pragma once

#include "pe/core/Gradient.hpp"

#include <QColor>
#include <QImage>
#include <QString>
#include <QVector>
#include <QWidget>

class QEvent;
class QListWidget;
class QObject;

namespace pe::app {

// The Gradients dock: ramps to draw with, and the one the Gradient tool is currently loaded
// with.
//
// The dock held a centred label. The tool underneath it could draw exactly one gradient,
// foreground to background, which is the one gradient that needs no panel at all: everything a
// Gradients panel exists to hold has stops in the middle, or fades to transparent, or is a
// named ramp somebody built once and wants back. So the engine grew pe::Gradient and this
// picks from it.
//
// Two of the presets follow the loaded colours rather than carrying their own (see
// pe::StopColor), so their swatches are redrawn when the foreground or background changes; the
// rest are fixed and drawn once. Each swatch is the real pe::Gradient sampled, over the same
// checkerboard the canvas uses, so a ramp that fades to transparent looks like one here too.
//
// Like the other panels it knows nothing about the document: it announces the chosen ramp and
// MainWindow loads it into the tool.
class GradientsPanel : public QWidget {
    Q_OBJECT

public:
    struct Preset {
        QString name;
        QString description;  // what it is for; the row's tooltip
        pe::Gradient gradient;
    };

    explicit GradientsPanel(QWidget* parent = nullptr);

    [[nodiscard]] int presetCount() const noexcept { return static_cast<int>(presets_.size()); }
    // The preset at `index`, or a default-constructed entry for an out-of-range index.
    [[nodiscard]] const Preset& preset(int index) const;

    // The colours the two dynamic presets follow. Redraws only the swatches that can change,
    // since most of the list cannot. Does not emit.
    void setColors(const QColor& foreground, const QColor& background);

    // The swatch for `index`, at `size`: the ramp sampled over a checkerboard. Public because
    // "the swatch shows the ramp it is labelled with" is the panel's whole claim, and it
    // cannot be asserted without being able to read one back.
    [[nodiscard]] QImage swatch(int index, QSize size) const;

    // What choosing a row does. One path for the mouse, the keyboard and the tests.
    void activate(int index);
    [[nodiscard]] int currentIndex() const noexcept { return current_; }

signals:
    void gradientChosen(int index);

protected:
    // Return/Enter/Space chooses the current row; QListWidget::itemActivated also fires for a
    // single click under some styles, which would load the ramp twice.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildPresets();
    void buildRows();
    void refreshDynamicSwatches();  // only the presets that follow the loaded colours

    QVector<Preset> presets_;
    QListWidget* list_ = nullptr;
    QColor foreground_{Qt::black};
    QColor background_{Qt::white};
    int current_ = 0;
};

}  // namespace pe::app
