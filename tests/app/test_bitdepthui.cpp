// Image > Mode. The conversion engine (SetBitDepthCommand, bitDepthBlocker) is tested headlessly;
// this covers the menu route and MainWindow::applyBitDepth, which has no dialog and is driven
// directly.

#include "MainWindow.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelFormat.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Refusal.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QString>

#include <cstddef>
#include <memory>

namespace {

QString plain(QString s) {
    return s.remove(QLatin1Char('&'));
}

QMenu* modeMenu(pe::app::MainWindow& w) {
    for (QAction* top : w.menuBar()->actions()) {
        if (top->menu() == nullptr || plain(top->text()) != QStringLiteral("Image")) continue;
        for (QAction* a : top->menu()->actions()) {
            if (a->menu() != nullptr && plain(a->text()) == QStringLiteral("Mode"))
                return a->menu();
        }
    }
    return nullptr;
}

bool hasAction(QMenu* menu, const QString& label) {
    if (menu == nullptr) return false;
    for (QAction* a : menu->actions()) {
        if (plain(a->text()) == label) return true;
    }
    return false;
}

std::unique_ptr<pe::Document> docWith(pe::Size size, pe::Rgba8 fill) {
    auto doc = pe::Document::createBlank(size);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, size.width, size.height}, fill);
    return doc;
}

}  // namespace

PE_TEST(bitdepthui_image_menu_has_a_mode_submenu) {
    pe::app::MainWindow w;
    QMenu* mode = modeMenu(w);
    PE_REQUIRE(mode != nullptr);
    PE_CHECK(hasAction(mode, QStringLiteral("8 Bits/Channel")));
    PE_CHECK(hasAction(mode, QStringLiteral("16 Bits/Channel")));
    PE_CHECK(hasAction(mode, QStringLiteral("32 Bits/Channel")));
}

PE_TEST(bitdepthui_apply_converts_the_document_undoable) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{16, 16}, pe::Rgba8{100, 150, 200, 255}), QString());
    PE_CHECK(w.document()->bitDepth() == pe::BitDepth::U8);
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_CHECK(w.applyBitDepth(pe::BitDepth::U16));
    PE_CHECK(w.document()->bitDepth() == pe::BitDepth::U16);
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore + 1);

    w.document()->history().undo();
    PE_CHECK(w.document()->bitDepth() == pe::BitDepth::U8);
}

PE_TEST(bitdepthui_apply_to_the_current_depth_is_refused) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{16, 16}, pe::Rgba8{10, 20, 30, 255}), QString());
    w.clearRefusals();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_CHECK(!w.applyBitDepth(pe::BitDepth::U8));  // already 8-bit
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore);
}

PE_TEST(bitdepthui_apply_without_a_document_is_refused) {
    pe::app::MainWindow w;
    w.clearRefusals();
    PE_CHECK(!w.applyBitDepth(pe::BitDepth::U16));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoDocument);
}
