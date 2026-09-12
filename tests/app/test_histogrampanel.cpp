// The Histogram dock. computeHistogram/channelStats are tested headlessly in the engine; this
// covers the panel that pulls the bounded composite and computes from it, plus its place in the
// window. Display only, so there is no command or undo to check.

#include "HistogramPanel.hpp"
#include "MainWindow.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Histogram.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe_test.hpp"

#include <QComboBox>
#include <QDockWidget>
#include <QString>

#include <memory>

using namespace pe;

namespace {
// Left half level 100, right half level 150 (opaque grey): two luma spikes to find in the bins.
PixelBuffer twoTone(int w, int h) {
    PixelBuffer img(w, h, Rgba8{150, 150, 150, 255});
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w / 2; ++x) img.set(x, y, Rgba8{100, 100, 100, 255});
    }
    return img;
}
}  // namespace

PE_TEST(histogrampanel_computes_the_bounded_composite_histogram) {
    auto doc = Document::createBlank(Size{20, 10});  // declared first: it must outlive the panel
    pe::app::HistogramPanel panel;
    const PixelBuffer img = twoTone(20, 10);
    panel.setPreviewSource([img](int) { return img; });
    panel.setDocument(doc.get());

    const Histogram& h = panel.histogram();
    PE_CHECK_EQ(static_cast<int>(h.count), 200);      // 20x10
    PE_CHECK_EQ(static_cast<int>(h.luma[100]), 100);  // left half
    PE_CHECK_EQ(static_cast<int>(h.luma[150]), 100);  // right half
    PE_CHECK_EQ(static_cast<int>(h.luma[125]), 0);    // nothing in between

    panel.setDocument(nullptr);  // detach before doc is destroyed
    PE_CHECK_EQ(static_cast<int>(panel.histogram().count), 0);
}

PE_TEST(histogrampanel_channel_selector_has_the_five_views) {
    pe::app::HistogramPanel panel;
    auto* channel = panel.findChild<QComboBox*>(QStringLiteral("HistogramChannel"));
    PE_REQUIRE(channel != nullptr);
    PE_CHECK_EQ(channel->count(), 5);  // Luminosity, RGB, Red, Green, Blue
}

PE_TEST(histogrampanel_is_empty_without_a_document) {
    pe::app::HistogramPanel panel;
    panel.setPreviewSource([](int) { return PixelBuffer(8, 8, Rgba8{10, 10, 10, 255}); });
    PE_CHECK_EQ(static_cast<int>(panel.histogram().count), 0);  // no document: nothing to measure
}

PE_TEST(histogrampanel_is_docked_in_the_window) {
    pe::app::MainWindow w;
    PE_CHECK(w.findChild<pe::app::HistogramPanel*>() != nullptr);
    bool hasDock = false;
    for (QDockWidget* d : w.findChildren<QDockWidget*>()) {
        if (d->objectName() == QStringLiteral("Histogram")) hasDock = true;
    }
    PE_CHECK(hasDock);
}
