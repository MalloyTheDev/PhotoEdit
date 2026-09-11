// Image > Canvas Size and Image > Crop to Selection. The engine command is tested headlessly
// in tests/core/test_canvassize.cpp; this covers the dialog that decides what to ask it for,
// and the menu route to Crop.

#include "CanvasSizeDialog.hpp"
#include "MainWindow.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Refusal.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <QAction>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QSpinBox>
#include <QString>
#include <QToolButton>

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

bool trigger(pe::app::MainWindow& w, const QString& label) {
    QAction* a = imageAction(w, label);
    if (a == nullptr) return false;
    a->trigger();
    return true;
}

std::unique_ptr<pe::Document> docWith(pe::Size size, pe::Rgba8 fill) {
    auto doc = pe::Document::createBlank(size);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, size.width, size.height}, fill);
    return doc;
}

QString summaryOf(pe::app::CanvasSizeDialog& dlg) {
    auto* l = dlg.findChild<QLabel*>(QStringLiteral("CanvasSizeSummary"));
    return l != nullptr ? l->text() : QString();
}

}  // namespace

PE_TEST(canvassizeui_the_image_menu_finally_has_more_than_adjustments) {
    // It held exactly one submenu. Both of these change the document's shape and neither had a
    // menu route: Canvas Size did not exist at all, and Crop was a drag with the Crop tool.
    pe::app::MainWindow w;
    QAction* canvas = imageAction(w, QStringLiteral("Canvas Size..."));
    PE_REQUIRE(canvas != nullptr);
    PE_CHECK(canvas->shortcut() == QKeySequence(QStringLiteral("Ctrl+Alt+C")));
    PE_CHECK(imageAction(w, QStringLiteral("Crop to Selection")) != nullptr);
}

PE_TEST(canvassize_dialog_opens_on_the_size_the_document_already_is) {
    // Opening on something else would make every trip through the dialog a resize, including
    // the ones where the user only wanted to change the anchor.
    pe::app::CanvasSizeDialog dlg(nullptr, pe::Size{800, 600});
    PE_CHECK(dlg.size() == (pe::Size{800, 600}));
    PE_CHECK(dlg.anchor() == pe::CanvasAnchor::Center);

    auto* current = dlg.findChild<QLabel*>(QStringLiteral("CanvasSizeCurrent"));
    PE_REQUIRE(current != nullptr);
    PE_CHECK(current->text().contains(QStringLiteral("800")));
    PE_CHECK(current->text().contains(QStringLiteral("600")));

    auto* width = dlg.findChild<QSpinBox*>(QStringLiteral("CanvasWidth"));
    PE_REQUIRE(width != nullptr);
    PE_CHECK_EQ(width->value(), 800);
    PE_CHECK_EQ(width->maximum(), pe::kMaxCanvasDimension);
    PE_CHECK_EQ(width->minimum(), 1);
}

PE_TEST(canvassize_dialog_anchor_grid_is_nine_buttons_with_exactly_one_chosen) {
    // The grid IS the explanation of what an anchor does, so it has to be a grid, and exactly
    // one of it can be true at a time.
    pe::app::CanvasSizeDialog dlg(nullptr, pe::Size{100, 100});
    QToolButton* buttons[9]{};
    for (int i = 0; i < 9; ++i) {
        buttons[i] = dlg.findChild<QToolButton*>(QStringLiteral("CanvasAnchor%1").arg(i));
        PE_REQUIRE(buttons[i] != nullptr);
        PE_CHECK(buttons[i]->isCheckable());
        // Reachable without sight of the arrow glyph.
        PE_CHECK(!buttons[i]->accessibleName().isEmpty());
    }

    const auto checkedCount = [&buttons] {
        int n = 0;
        for (auto* b : buttons) {
            if (b->isChecked()) ++n;
        }
        return n;
    };
    PE_CHECK_EQ(checkedCount(), 1);
    PE_CHECK(buttons[static_cast<int>(pe::CanvasAnchor::Center)]->isChecked());

    buttons[static_cast<int>(pe::CanvasAnchor::TopLeft)]->click();
    PE_CHECK(dlg.anchor() == pe::CanvasAnchor::TopLeft);
    PE_CHECK_EQ(checkedCount(), 1);
    PE_CHECK(buttons[static_cast<int>(pe::CanvasAnchor::TopLeft)]->isChecked());

    buttons[static_cast<int>(pe::CanvasAnchor::BottomRight)]->click();
    PE_CHECK(dlg.anchor() == pe::CanvasAnchor::BottomRight);
    PE_CHECK_EQ(checkedCount(), 1);
}

PE_TEST(canvassize_dialog_says_which_edges_change_and_by_how_much) {
    // Two numbers in two boxes do not tell anyone where the space is going. The anchor does,
    // and this is the only place that says so in words.
    pe::app::CanvasSizeDialog dlg(nullptr, pe::Size{100, 100});
    PE_CHECK(summaryOf(dlg).contains(QStringLiteral("nothing would change")));

    dlg.setSize(pe::Size{200, 200});
    dlg.setAnchor(pe::CanvasAnchor::Center);
    QString text = summaryOf(dlg);
    PE_CHECK(text.contains(QStringLiteral("Left 50 added")));
    PE_CHECK(text.contains(QStringLiteral("right 50 added")));

    dlg.setAnchor(pe::CanvasAnchor::TopLeft);
    text = summaryOf(dlg);
    PE_CHECK(text.contains(QStringLiteral("Left 0 added")));
    PE_CHECK(text.contains(QStringLiteral("right 100 added")));

    // Shrinking says cut, not "added -50".
    dlg.setSize(pe::Size{50, 50});
    dlg.setAnchor(pe::CanvasAnchor::Center);
    text = summaryOf(dlg);
    PE_CHECK(text.contains(QStringLiteral("cut")));
    PE_CHECK(!text.contains(QStringLiteral("-")));
    // And it says the thing a user is most likely to be afraid of.
    PE_CHECK(text.contains(QStringLiteral("not deleted")));
}

PE_TEST(canvassizeui_crop_to_selection_crops_to_the_selection) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{64, 64}, pe::Rgba8{200, 100, 50, 255}), QString());
    pe::Selection sel;
    sel.selectRect(pe::Rect{8, 8, 16, 16});
    w.document()->history().push(std::make_unique<pe::SetSelectionCommand>(std::move(sel)));
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_REQUIRE(trigger(w, QStringLiteral("Crop to Selection")));

    PE_CHECK(w.document()->canvasSize() == (pe::Size{16, 16}));
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore + 1);
    // The kept region moved to the origin, so what was at (8,8) is at (0,0).
    const auto* pl =
        static_cast<const pe::PixelLayer*>(w.document()->findLayer(w.document()->activeLayer()));
    PE_CHECK_EQ(pl->tiles().pixel(0, 0).r, static_cast<uint8_t>(200));

    w.document()->history().undo();
    PE_CHECK(w.document()->canvasSize() == (pe::Size{64, 64}));
}

PE_TEST(canvassizeui_crop_to_selection_without_a_selection_is_refused) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{64, 64}, pe::Rgba8{200, 100, 50, 255}), QString());
    w.clearRefusals();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_REQUIRE(trigger(w, QStringLiteral("Crop to Selection")));

    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoSelection);
    PE_REQUIRE(!w.refusals().empty());
    // Names the other way of doing it, since the Crop tool is right there.
    PE_CHECK(w.refusals().back().explanation.find("Crop tool") != std::string::npos);
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore);
}

PE_TEST(canvassizeui_crop_to_a_selection_covering_everything_is_refused) {
    // It would be a history entry that changed nothing, which the user then has to undo.
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{64, 64}, pe::Rgba8{200, 100, 50, 255}), QString());
    pe::Selection sel;
    sel.selectRect(pe::Rect{0, 0, 64, 64});
    w.document()->history().push(std::make_unique<pe::SetSelectionCommand>(std::move(sel)));
    w.clearRefusals();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_REQUIRE(trigger(w, QStringLiteral("Crop to Selection")));

    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore);
}

PE_TEST(canvassizeui_crop_to_a_selection_that_selects_nothing_is_refused) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{64, 64}, pe::Rgba8{200, 100, 50, 255}), QString());
    pe::Selection sel;
    sel.selectRect(pe::Rect{4, 4, 8, 8});
    sel.subtractRect(pe::Rect{0, 0, 64, 64});
    w.document()->history().push(std::make_unique<pe::SetSelectionCommand>(std::move(sel)));
    w.clearRefusals();

    PE_REQUIRE(trigger(w, QStringLiteral("Crop to Selection")));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
    PE_CHECK(w.document()->canvasSize() == (pe::Size{64, 64}));
}
