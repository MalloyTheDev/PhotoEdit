#pragma once

#include "pe/core/Document.hpp"
#include "pe/core/Histogram.hpp"
#include "pe/core/PixelBuffer.hpp"

#include <QWidget>

#include <functional>

class QComboBox;
class QLabel;
class QShowEvent;

namespace pe::app {

// The drawing surface for HistogramPanel: 256-bin bars for one channel, or the three RGB channels
// overlaid additively. Display only, and it declares no signals or slots, so it needs no Q_OBJECT.
class HistogramView : public QWidget {
public:
    explicit HistogramView(QWidget* parent = nullptr);
    // mode: 0 Luminosity, 1 RGB, 2 Red, 3 Green, 4 Blue.
    void setData(const pe::Histogram& hist, int mode);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    pe::Histogram hist_{};
    int mode_ = 0;
};

// The Histogram dock: the tonal distribution of the composite, per channel.
//
// Reads the same bounded, downscaled composite the Channels dock uses (never
// Document::compositeImage(), which returns nothing past its megapixel cap, #146), so it keeps
// working on large documents; a downscaled sample is statistically the same histogram. Display
// only: nothing here edits the document or touches the undo stack. Computed on open and, while on
// screen, on any change that can move a pixel; a change that arrives while hidden is caught up on
// show (#156, the same lesson as the Channels thumbnails).
class HistogramPanel : public QWidget, public pe::DocumentObserver {
    Q_OBJECT

public:
    explicit HistogramPanel(QWidget* parent = nullptr);
    ~HistogramPanel() override;

    // Observe and show `doc`, or detach and clear when null. Must be called with null before the
    // observed document is destroyed.
    void setDocument(pe::Document* doc);
    // Where the data comes from: a bounded, downscaled composite (at most `maxPixels` out) from the
    // canvas renderer's cache rather than a full flatten. The same source the Channels dock uses.
    void setPreviewSource(std::function<pe::PixelBuffer(int maxPixels)> source);

    // The last histogram computed. For tests, and for any readout built on it.
    [[nodiscard]] const pe::Histogram& histogram() const noexcept { return hist_; }

    void onDocumentChanged(const pe::Document&, const pe::DocumentChange&) override;

protected:
    void showEvent(QShowEvent* e) override;

private:
    void refresh();       // pull one bounded preview and recompute
    void refreshStats();  // the mean/median/std-dev readout for the current channel

    pe::Document* doc_ = nullptr;  // not owned; observed while non-null
    std::function<pe::PixelBuffer(int)> previewSource_;
    pe::Histogram hist_{};
    bool stale_ = true;
    QComboBox* channel_ = nullptr;
    HistogramView* view_ = nullptr;
    QLabel* stats_ = nullptr;
};

}  // namespace pe::app
