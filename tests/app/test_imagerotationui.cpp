// Image > Image Rotation and Image > Trim. The engine (OrientDocumentCommand, orientBlocker, and
// Crop for Trim) is tested headlessly; this covers the menu route and MainWindow::applyOrient /
// trimTransparent, which have no dialog and so are driven directly.

#include "MainWindow.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Orient.hpp"
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

QMenu* imageMenu(pe::app::MainWindow& w) {
    for (QAction* top : w.menuBar()->actions()) {
        if (top->menu() != nullptr && plain(top->text()) == QStringLiteral("Image")) {
            return top->menu();
        }
    }
    return nullptr;
}

QMenu* submenu(QMenu* menu, const QString& label) {
    if (menu == nullptr) return nullptr;
    for (QAction* a : menu->actions()) {
        if (a->menu() != nullptr && plain(a->text()) == label) return a->menu();
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

pe::PixelLayer* base(pe::Document& doc) {
    return static_cast<pe::PixelLayer*>(doc.findLayer(doc.activeLayer()));
}

}  // namespace

PE_TEST(imagerotationui_menu_has_the_rotation_family_and_trim) {
    pe::app::MainWindow w;
    QMenu* rot = submenu(imageMenu(w), QStringLiteral("Image Rotation"));
    PE_REQUIRE(rot != nullptr);
    PE_CHECK(hasAction(rot, QStringLiteral("Rotate 90 CW")));
    PE_CHECK(hasAction(rot, QStringLiteral("Rotate 90 CCW")));
    PE_CHECK(hasAction(rot, QStringLiteral("Rotate 180")));
    PE_CHECK(hasAction(rot, QStringLiteral("Flip Horizontal")));
    PE_CHECK(hasAction(rot, QStringLiteral("Flip Vertical")));
    PE_CHECK(hasAction(imageMenu(w), QStringLiteral("Trim")));
}

PE_TEST(imagerotationui_apply_rotate90_swaps_the_canvas_undoable) {
    auto doc = pe::Document::createBlank(pe::Size{40, 24});
    base(*doc)->tiles().setPixel(0, 0, pe::Rgba8{200, 50, 50, 255});
    pe::app::MainWindow w;
    w.setDocument(std::move(doc), QString());
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_CHECK(w.applyOrient(pe::Orient::Rotate90CW));
    PE_CHECK(w.document()->canvasSize() == (pe::Size{24, 40}));  // sides swapped
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore + 1);

    w.document()->history().undo();
    PE_CHECK(w.document()->canvasSize() == (pe::Size{40, 24}));
}

PE_TEST(imagerotationui_apply_without_a_document_is_refused) {
    pe::app::MainWindow w;
    w.clearRefusals();
    PE_CHECK(!w.applyOrient(pe::Orient::FlipHorizontal));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoDocument);
}

PE_TEST(imagerotationui_trim_crops_to_the_content_undoable) {
    auto doc = pe::Document::createBlank(pe::Size{20, 20});  // transparent canvas
    base(*doc)->tiles().fillRect(pe::Rect{4, 4, 8, 8}, pe::Rgba8{200, 100, 50, 255});
    pe::app::MainWindow w;
    w.setDocument(std::move(doc), QString());
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_CHECK(w.trimTransparent());
    PE_CHECK(w.document()->canvasSize() == (pe::Size{8, 8}));  // cropped to the 8x8 content
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore + 1);
    // The kept content moved to the origin: what was at (4,4) is now at (0,0).
    PE_CHECK_EQ(base(*w.document())->tiles().pixel(0, 0).a, static_cast<uint8_t>(255));

    w.document()->history().undo();
    PE_CHECK(w.document()->canvasSize() == (pe::Size{20, 20}));
}

PE_TEST(imagerotationui_trim_with_no_transparent_border_is_refused) {
    auto doc = pe::Document::createBlank(pe::Size{16, 16});
    base(*doc)->tiles().fillRect(pe::Rect{0, 0, 16, 16}, pe::Rgba8{10, 20, 30, 255});  // fills all
    pe::app::MainWindow w;
    w.setDocument(std::move(doc), QString());
    w.clearRefusals();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_CHECK(!w.trimTransparent());
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore);
}

PE_TEST(imagerotationui_trim_a_fully_transparent_image_is_refused) {
    auto doc = pe::Document::createBlank(pe::Size{16, 16});  // nothing painted
    pe::app::MainWindow w;
    w.setDocument(std::move(doc), QString());
    w.clearRefusals();
    PE_CHECK(!w.trimTransparent());
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
}
