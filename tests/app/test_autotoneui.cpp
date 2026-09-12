// Image > Auto Tone / Auto Contrast. The stretch itself (computeAutoTone/applyAutoTone) is tested
// headlessly in the engine; this covers the menu route and MainWindow::autoAdjust, which derives
// the endpoints from the composite histogram and bakes them onto the active layer. No dialog, so
// autoAdjust is driven directly.

#include "MainWindow.hpp"
#include "pe/core/AutoTone.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Refusal.hpp"
#include "pe/core/SolidColorLayer.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QString>

#include <cstddef>
#include <memory>

namespace {

QString plain(QString s) {
    return s.remove(QLatin1Char('&'));
}

QAction* imageAction(pe::app::MainWindow& w, const QString& label) {
    for (QAction* top : w.menuBar()->actions()) {
        if (top->menu() == nullptr || plain(top->text()) != QStringLiteral("Image")) continue;
        for (QAction* a : top->menu()->actions()) {
            if (plain(a->text()) == label) return a;
        }
    }
    return nullptr;
}

// A low-contrast two-tone image: left half level 100, right half level 150 (opaque grey). Auto
// Contrast/Tone should stretch that [100,150] range out toward [0,255].
std::unique_ptr<pe::Document> twoTone(int w, int h) {
    auto doc = pe::Document::createBlank(pe::Size{w, h});
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, w / 2, h}, pe::Rgba8{100, 100, 100, 255});
    pl->tiles().fillRect(pe::Rect{w / 2, 0, w - w / 2, h}, pe::Rgba8{150, 150, 150, 255});
    return doc;
}

const pe::PixelLayer* pixels(const pe::Document& doc) {
    return static_cast<const pe::PixelLayer*>(doc.findLayer(doc.activeLayer()));
}

}  // namespace

PE_TEST(autotoneui_image_menu_has_auto_tone_and_contrast) {
    pe::app::MainWindow w;
    QAction* tone = imageAction(w, QStringLiteral("Auto Tone"));
    QAction* contrast = imageAction(w, QStringLiteral("Auto Contrast"));
    PE_REQUIRE(tone != nullptr);
    PE_REQUIRE(contrast != nullptr);
    PE_CHECK(tone->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    PE_CHECK(contrast->shortcut() == QKeySequence(QStringLiteral("Ctrl+Alt+Shift+L")));
}

PE_TEST(autotoneui_auto_contrast_stretches_the_range_undoable) {
    auto doc = twoTone(32, 16);
    pe::app::MainWindow w;
    w.setDocument(std::move(doc), QString());
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_CHECK(w.autoAdjust(pe::AutoToneMode::Contrast));
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore + 1);
    // The dark half was pushed toward black and the light half toward white.
    PE_CHECK(pixels(*w.document())->tiles().pixel(4, 8).r < 40);
    PE_CHECK(pixels(*w.document())->tiles().pixel(28, 8).r > 210);

    w.document()->history().undo();
    PE_CHECK_EQ(pixels(*w.document())->tiles().pixel(4, 8).r, static_cast<uint8_t>(100));
    PE_CHECK_EQ(pixels(*w.document())->tiles().pixel(28, 8).r, static_cast<uint8_t>(150));
}

PE_TEST(autotoneui_auto_tone_per_channel_also_stretches) {
    // On a neutral grey image the per-channel (Levels) mode stretches each channel the same way as
    // Contrast, so the same widening is the visible result; this pins that the Levels menu path is
    // wired to the same engine.
    auto doc = twoTone(32, 16);
    pe::app::MainWindow w;
    w.setDocument(std::move(doc), QString());

    PE_CHECK(w.autoAdjust(pe::AutoToneMode::Levels));
    PE_CHECK(pixels(*w.document())->tiles().pixel(4, 8).r < 40);
    PE_CHECK(pixels(*w.document())->tiles().pixel(28, 8).r > 210);
}

PE_TEST(autotoneui_a_flat_image_has_no_range_to_stretch) {
    auto doc = pe::Document::createBlank(pe::Size{16, 16});
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, 16, 16}, pe::Rgba8{120, 120, 120, 255});
    pe::app::MainWindow w;
    w.setDocument(std::move(doc), QString());
    w.clearRefusals();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_CHECK(!w.autoAdjust(pe::AutoToneMode::Contrast));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore);  // no dead history entry
}

PE_TEST(autotoneui_a_non_pixel_active_layer_is_refused) {
    auto doc = twoTone(16, 16);
    auto fill =
        std::make_unique<pe::SolidColorLayer>(pe::Rgba8{0, 0, 0, 255}, pe::Rect{0, 0, 16, 16});
    const pe::LayerId fillId = fill->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(fill));
    doc->setActiveLayer(fillId);
    pe::app::MainWindow w;
    w.setDocument(std::move(doc), QString());
    w.clearRefusals();

    PE_CHECK(!w.autoAdjust(pe::AutoToneMode::Contrast));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::LayerNotPixel);
}

PE_TEST(autotoneui_without_a_document_is_refused) {
    pe::app::MainWindow w;  // no document
    w.clearRefusals();
    PE_CHECK(!w.autoAdjust(pe::AutoToneMode::Contrast));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoDocument);
}
