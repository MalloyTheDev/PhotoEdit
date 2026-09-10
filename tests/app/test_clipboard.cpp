// Cut, Copy, Copy Merged, Paste, Paste Into and Clear. None of this existed: the Edit menu was
// Undo, Redo and Free Transform, so Ctrl+C and Ctrl+V did nothing at all.

#include "CanvasView.hpp"
#include "MainWindow.hpp"
#include "pe/core/Adjustment.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Refusal.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <QAction>
#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QKeySequence>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QString>

#include <cstddef>
#include <memory>

namespace {

QString plain(QString s) {
    return s.remove(QLatin1Char('&'));
}

QAction* editAction(pe::app::MainWindow& w, const QString& label) {
    for (QAction* top : w.menuBar()->actions()) {
        if (top->menu() == nullptr || plain(top->text()) != QStringLiteral("Edit")) continue;
        for (QAction* a : top->menu()->actions()) {
            if (plain(a->text()) == label) return a;
        }
    }
    return nullptr;
}

bool trigger(pe::app::MainWindow& w, const QString& label) {
    QAction* a = editAction(w, label);
    if (a == nullptr) return false;
    a->trigger();
    return true;
}

std::unique_ptr<pe::Document> docWith(pe::Rgba8 fill, pe::Size size = pe::Size{64, 64}) {
    auto doc = pe::Document::createBlank(size);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, size.width, size.height}, fill);
    return doc;
}

void selectRect(pe::Document& doc, pe::Rect r) {
    pe::Selection sel;
    sel.selectRect(r);
    doc.history().push(std::make_unique<pe::SetSelectionCommand>(std::move(sel)));
}

void putOnClipboard(QImage img) {
    QGuiApplication::clipboard()->setImage(img);
}

QImage solidImage(int w, int h, QColor c) {
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(c);
    return img;
}

}  // namespace

PE_TEST(clipboard_the_edit_menu_carries_the_conventional_entries_and_shortcuts) {
    // The shortcuts are the whole point: nobody opens a menu to copy. Asserting on them here
    // means a rename or a reorder cannot quietly drop one.
    pe::app::MainWindow w;
    const std::pair<QString, QKeySequence::StandardKey> standard[] = {
        {QStringLiteral("Cut"), QKeySequence::Cut},
        {QStringLiteral("Copy"), QKeySequence::Copy},
        {QStringLiteral("Paste"), QKeySequence::Paste},
    };
    for (const auto& [label, key] : standard) {
        QAction* a = editAction(w, label);
        PE_REQUIRE(a != nullptr);
        PE_CHECK(!a->shortcut().isEmpty());
        const QKeySequence expected(key);
        if (!expected.isEmpty()) PE_CHECK(a->shortcut() == expected);
    }
    QAction* merged = editAction(w, QStringLiteral("Copy Merged"));
    PE_REQUIRE(merged != nullptr);
    PE_CHECK(merged->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+C")));
    QAction* into = editAction(w, QStringLiteral("Paste Into"));
    PE_REQUIRE(into != nullptr);
    PE_CHECK(into->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+V")));
    PE_CHECK(editAction(w, QStringLiteral("Clear")) != nullptr);
}

PE_TEST(clipboard_copy_takes_the_selection_and_nothing_else) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{200, 100, 50, 255}), QString());
    selectRect(*w.document(), pe::Rect{8, 8, 16, 12});
    putOnClipboard(QImage());  // start from nothing, so a pass cannot be a stale clipboard

    PE_REQUIRE(trigger(w, QStringLiteral("Copy")));

    const QImage got = QGuiApplication::clipboard()->image();
    PE_REQUIRE(!got.isNull());
    PE_CHECK_EQ(got.width(), 16);  // the selection's size, not the canvas's
    PE_CHECK_EQ(got.height(), 12);
    const QColor px = got.pixelColor(4, 4);
    PE_CHECK_EQ(px.red(), 200);
    PE_CHECK_EQ(px.green(), 100);
    PE_CHECK_EQ(px.alpha(), 255);
}

PE_TEST(clipboard_copy_with_no_selection_takes_the_whole_canvas) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{10, 20, 30, 255}, pe::Size{40, 24}), QString());
    putOnClipboard(QImage());

    PE_REQUIRE(trigger(w, QStringLiteral("Copy")));

    const QImage got = QGuiApplication::clipboard()->image();
    PE_REQUIRE(!got.isNull());
    PE_CHECK_EQ(got.width(), 40);
    PE_CHECK_EQ(got.height(), 24);
}

PE_TEST(clipboard_copy_from_a_layer_with_no_pixels_is_refused_and_says_what_to_do) {
    // An adjustment layer has no pixels to copy. Saying nothing would look like a broken
    // Ctrl+C; the refusal names Copy Merged, which is what the user actually wants here.
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{10, 20, 30, 255}), QString());
    auto adj = std::make_unique<pe::AdjustmentLayer>(std::make_unique<pe::Invert>(), "Negative");
    const pe::LayerId id = adj->id();
    w.document()->history().push(
        std::make_unique<pe::AddLayerCommand>(std::move(adj), w.document()->topLevelCount()));
    w.document()->setActiveLayer(id);
    w.clearRefusals();

    PE_REQUIRE(trigger(w, QStringLiteral("Copy")));
    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(1));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::LayerNotPixel);
    PE_REQUIRE(!w.refusals().empty());
    PE_CHECK(w.refusals().back().explanation.find("Copy Merged") != std::string::npos);
}

PE_TEST(clipboard_copy_merged_takes_the_composite_rather_than_the_active_layer) {
    // The difference between the two copies, on a document where they cannot coincide: the
    // active layer is a small patch over a full-canvas backdrop.
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{0, 0, 255, 255}), QString());  // blue backdrop
    auto patch = std::make_unique<pe::PixelLayer>("Patch");
    patch->tiles().fillRect(pe::Rect{0, 0, 8, 8}, pe::Rgba8{255, 0, 0, 255});
    const pe::LayerId id = patch->id();
    w.document()->history().push(
        std::make_unique<pe::AddLayerCommand>(std::move(patch), w.document()->topLevelCount()));
    w.document()->setActiveLayer(id);

    PE_REQUIRE(trigger(w, QStringLiteral("Copy")));
    const QImage layerOnly = QGuiApplication::clipboard()->image();
    PE_REQUIRE(!layerOnly.isNull());
    PE_CHECK_EQ(layerOnly.pixelColor(4, 4).red(), 255);
    PE_CHECK_EQ(layerOnly.pixelColor(40, 40).alpha(), 0);  // the patch does not reach there

    PE_REQUIRE(trigger(w, QStringLiteral("Copy Merged")));
    const QImage merged = QGuiApplication::clipboard()->image();
    PE_REQUIRE(!merged.isNull());
    PE_CHECK_EQ(merged.pixelColor(4, 4).red(), 255);     // the patch is still on top
    PE_CHECK_EQ(merged.pixelColor(40, 40).blue(), 255);  // and the backdrop came with it
    PE_CHECK_EQ(merged.pixelColor(40, 40).alpha(), 255);
}

PE_TEST(clipboard_paste_adds_one_undoable_layer_holding_the_clipboard_s_pixels) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{10, 20, 30, 255}), QString());
    putOnClipboard(solidImage(16, 16, QColor(0, 200, 0)));

    const std::size_t layersBefore = w.document()->topLevelCount();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_REQUIRE(trigger(w, QStringLiteral("Paste")));

    PE_CHECK_EQ(w.document()->topLevelCount(), layersBefore + 1);
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore + 1);

    const pe::Layer* added = w.document()->findLayer(w.document()->activeLayer());
    PE_REQUIRE(added != nullptr);
    PE_CHECK(added->kind() == pe::LayerKind::Pixel);
    const auto* pl = static_cast<const pe::PixelLayer*>(added);
    // Centred on the canvas: a 16px square on a 64px canvas starts at 24.
    PE_CHECK_EQ(pl->tiles().pixel(32, 32).g, static_cast<uint8_t>(200));
    PE_CHECK_EQ(pl->tiles().pixel(4, 4).a, static_cast<uint8_t>(0));

    w.document()->history().undo();
    PE_CHECK_EQ(w.document()->topLevelCount(), layersBefore);
}

PE_TEST(clipboard_paste_with_nothing_on_the_clipboard_is_refused) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{10, 20, 30, 255}), QString());
    putOnClipboard(QImage());
    w.clearRefusals();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_REQUIRE(trigger(w, QStringLiteral("Paste")));

    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(1));
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore);  // no empty layer added
}

PE_TEST(clipboard_paste_into_masks_the_pasted_pixels_by_the_selection) {
    // Paste Into is not a crop: every pasted pixel arrives, and the selection becomes a mask,
    // so the paste can still be moved around inside the shape afterwards.
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{10, 20, 30, 255}), QString());
    selectRect(*w.document(), pe::Rect{16, 16, 32, 32});
    // Deliberately LARGER than the selection, so the paste overhangs it. That is the case the
    // mask exists for, and the case where "paste into" and "crop to the selection" differ: a
    // pasted image the same size as the hole cannot tell the two apart.
    putOnClipboard(solidImage(48, 48, QColor(0, 200, 0)));

    PE_REQUIRE(trigger(w, QStringLiteral("Paste Into")));

    const pe::Layer* added = w.document()->findLayer(w.document()->activeLayer());
    PE_REQUIRE(added != nullptr);
    PE_REQUIRE(added->mask() != nullptr);
    const auto* pl = static_cast<const pe::PixelLayer*>(added);
    // Centred on the selection, so the 48px square starts at (8,8) and overhangs on all sides.
    // Every pixel is present, inside the selection and out.
    PE_CHECK_EQ(pl->tiles().pixel(20, 20).g, static_cast<uint8_t>(200));
    PE_CHECK_EQ(pl->tiles().pixel(10, 10).g, static_cast<uint8_t>(200));
    // The mask is what decides which of them shows.
    PE_CHECK(added->mask()->evaluate(20, 20) > 0.99f);
    PE_CHECK(added->mask()->evaluate(10, 10) < 0.01f);
}

PE_TEST(clipboard_paste_into_without_a_selection_is_refused_rather_than_pasting_anyway) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{10, 20, 30, 255}), QString());
    putOnClipboard(solidImage(8, 8, QColor(0, 200, 0)));
    w.clearRefusals();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_REQUIRE(trigger(w, QStringLiteral("Paste Into")));

    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoSelection);
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore);
}

PE_TEST(clipboard_clear_empties_the_selection_and_undoes_back) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{200, 100, 50, 255}), QString());
    const pe::LayerId id = w.document()->activeLayer();
    selectRect(*w.document(), pe::Rect{8, 8, 16, 16});
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_REQUIRE(trigger(w, QStringLiteral("Clear")));

    const auto* pl = static_cast<const pe::PixelLayer*>(w.document()->findLayer(id));
    PE_CHECK_EQ(pl->tiles().pixel(12, 12).a, static_cast<uint8_t>(0));
    PE_CHECK_EQ(pl->tiles().pixel(40, 40).a, static_cast<uint8_t>(255));  // outside is untouched
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore + 1);

    w.document()->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(12, 12).a, static_cast<uint8_t>(255));
}

PE_TEST(clipboard_cut_copies_and_then_clears) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{200, 100, 50, 255}), QString());
    const pe::LayerId id = w.document()->activeLayer();
    selectRect(*w.document(), pe::Rect{8, 8, 16, 16});
    putOnClipboard(QImage());

    PE_REQUIRE(trigger(w, QStringLiteral("Cut")));

    const QImage got = QGuiApplication::clipboard()->image();
    PE_REQUIRE(!got.isNull());
    PE_CHECK_EQ(got.width(), 16);
    PE_CHECK_EQ(got.pixelColor(4, 4).red(), 200);  // it was copied before it was cleared

    const auto* pl = static_cast<const pe::PixelLayer*>(w.document()->findLayer(id));
    PE_CHECK_EQ(pl->tiles().pixel(12, 12).a, static_cast<uint8_t>(0));
}

PE_TEST(clipboard_a_cut_that_cannot_copy_does_not_clear) {
    // The ordering that matters. Clearing first and then failing to copy destroys pixels the
    // user believes are on the clipboard, and one undo does not obviously get them back.
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{200, 100, 50, 255}), QString());
    auto adj = std::make_unique<pe::AdjustmentLayer>(std::make_unique<pe::Invert>(), "Negative");
    const pe::LayerId adjId = adj->id();
    w.document()->history().push(
        std::make_unique<pe::AddLayerCommand>(std::move(adj), w.document()->topLevelCount()));
    w.document()->setActiveLayer(adjId);
    w.clearRefusals();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_REQUIRE(trigger(w, QStringLiteral("Cut")));

    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::LayerNotPixel);
    // Nothing was cleared: the cut stopped at the refused copy.
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore);
}

PE_TEST(clipboard_copy_of_a_selection_that_selects_nothing_is_refused) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{200, 100, 50, 255}), QString());
    pe::Selection sel;
    sel.selectRect(pe::Rect{4, 4, 8, 8});
    sel.subtractRect(pe::Rect{0, 0, 64, 64});
    w.document()->history().push(std::make_unique<pe::SetSelectionCommand>(std::move(sel)));
    w.clearRefusals();

    PE_REQUIRE(trigger(w, QStringLiteral("Copy")));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
}

PE_TEST(clipboard_a_copy_and_paste_round_trip_reproduces_the_pixels) {
    // What the whole feature is for, through the real menu actions and the real clipboard.
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{123, 45, 67, 255}), QString());
    selectRect(*w.document(), pe::Rect{10, 10, 20, 20});

    PE_REQUIRE(trigger(w, QStringLiteral("Copy")));
    PE_REQUIRE(trigger(w, QStringLiteral("Paste")));

    const pe::Layer* added = w.document()->findLayer(w.document()->activeLayer());
    PE_REQUIRE(added != nullptr);
    const auto* pl = static_cast<const pe::PixelLayer*>(added);
    // A 20px square centred on a 64px canvas starts at 22.
    const pe::Rgba8 px = pl->tiles().pixel(30, 30);
    PE_CHECK_EQ(px.r, static_cast<uint8_t>(123));
    PE_CHECK_EQ(px.g, static_cast<uint8_t>(45));
    PE_CHECK_EQ(px.b, static_cast<uint8_t>(67));
    PE_CHECK_EQ(px.a, static_cast<uint8_t>(255));
}

PE_TEST(clipboard_copy_merged_is_shaped_by_the_selection_too) {
    // The composite arrives from the renderer as a rectangle. If the selection were not folded
    // into it, Copy Merged would quietly paste a square where a plain Copy pastes a shape, and
    // the two would disagree about what "copy" means.
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Rgba8{0, 0, 255, 255}), QString());
    pe::Selection sel;
    sel.selectRect(pe::Rect{8, 8, 32, 32});
    sel.feather(4.0f, w.document()->canvasBounds());
    w.document()->history().push(std::make_unique<pe::SetSelectionCommand>(std::move(sel)));

    PE_REQUIRE(trigger(w, QStringLiteral("Copy Merged")));
    const QImage got = QGuiApplication::clipboard()->image();
    PE_REQUIRE(!got.isNull());
    // Solid in the middle of the selection, soft at its edge.
    PE_CHECK_EQ(got.pixelColor(got.width() / 2, got.height() / 2).alpha(), 255);
    PE_CHECK(got.pixelColor(0, got.height() / 2).alpha() < 255);
}
