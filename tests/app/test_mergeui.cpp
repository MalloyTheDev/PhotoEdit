// Merge Down, Merge Visible and Flatten Image from the Layer menu. The engine command is
// tested headlessly in tests/core/test_merge.cpp; this is the part that decides WHICH layers
// each entry takes, and what it says when it cannot run.

#include "MainWindow.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Refusal.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <QAction>
#include <QKeySequence>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QString>

#include <cstddef>
#include <memory>
#include <vector>

namespace {

QString plain(QString s) {
    return s.remove(QLatin1Char('&'));
}

QAction* layerAction(pe::app::MainWindow& w, const QString& label) {
    for (QAction* top : w.menuBar()->actions()) {
        if (top->menu() == nullptr || plain(top->text()) != QStringLiteral("Layer")) continue;
        for (QAction* a : top->menu()->actions()) {
            if (plain(a->text()) == label) return a;
        }
    }
    return nullptr;
}

bool trigger(pe::app::MainWindow& w, const QString& label) {
    QAction* a = layerAction(w, label);
    if (a == nullptr) return false;
    a->trigger();
    return true;
}

std::unique_ptr<pe::Document> stackOf(std::vector<pe::Rgba8> colours) {
    auto doc = pe::Document::createBlank(pe::Size{32, 32});
    auto* first = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    first->tiles().fillRect(pe::Rect{0, 0, 32, 32}, colours.front());
    first->setName("Base");
    for (std::size_t i = 1; i < colours.size(); ++i) {
        auto l = std::make_unique<pe::PixelLayer>("L" + std::to_string(i));
        l->tiles().fillRect(pe::Rect{0, 0, 32, 32}, colours[i]);
        doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(l));
    }
    return doc;
}

pe::Rgba8 pixelOf(const pe::Layer* l, int x, int y) {
    return static_cast<const pe::PixelLayer*>(l)->tiles().pixel(x, y);
}

}  // namespace

PE_TEST(mergeui_the_layer_menu_carries_the_three_merges) {
    pe::app::MainWindow w;
    QAction* down = layerAction(w, QStringLiteral("Merge Down"));
    PE_REQUIRE(down != nullptr);
    PE_CHECK(down->shortcut() == QKeySequence(QStringLiteral("Ctrl+E")));

    QAction* visible = layerAction(w, QStringLiteral("Merge Visible"));
    PE_REQUIRE(visible != nullptr);
    PE_CHECK(visible->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+E")));

    // Flatten deliberately has none: it throws away the most and should take a trip to the menu.
    QAction* flatten = layerAction(w, QStringLiteral("Flatten Image"));
    PE_REQUIRE(flatten != nullptr);
    PE_CHECK(flatten->shortcut().isEmpty());
}

PE_TEST(mergeui_merge_down_combines_the_active_layer_with_the_one_under_it) {
    pe::app::MainWindow w;
    w.setDocument(stackOf({pe::Rgba8{255, 0, 0, 255}, pe::Rgba8{0, 0, 255, 255}}), QString());
    auto* doc = w.document();
    doc->setActiveLayer(doc->topLevelLayers()[1]->id());
    const std::size_t undoBefore = doc->history().undoDepth();

    PE_REQUIRE(trigger(w, QStringLiteral("Merge Down")));

    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));
    PE_CHECK_EQ(doc->history().undoDepth(), undoBefore + 1);
    PE_CHECK(pixelOf(doc->topLevelLayers()[0].get(), 8, 8).b > 250);
    // The survivor keeps the LOWER layer's name: it is the one that stayed where it was.
    PE_CHECK(doc->topLevelLayers()[0]->name() == "Base");

    doc->history().undo();
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));
}

PE_TEST(mergeui_merge_down_at_the_bottom_of_the_stack_is_refused_and_says_why) {
    // Nothing under the bottom layer. Doing nothing quietly is what makes a menu entry look
    // broken; this is the case a user hits by accident most often.
    pe::app::MainWindow w;
    w.setDocument(stackOf({pe::Rgba8{255, 0, 0, 255}, pe::Rgba8{0, 0, 255, 255}}), QString());
    auto* doc = w.document();
    doc->setActiveLayer(doc->topLevelLayers()[0]->id());  // the bottom one
    w.clearRefusals();
    const std::size_t undoBefore = doc->history().undoDepth();

    PE_REQUIRE(trigger(w, QStringLiteral("Merge Down")));

    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(1));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
    PE_REQUIRE(!w.refusals().empty());
    PE_CHECK(w.refusals().back().explanation.find("under this one") != std::string::npos);
    // And no history entry to undo: a refused merge is not an edit.
    PE_CHECK_EQ(doc->history().undoDepth(), undoBefore);
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));
}

PE_TEST(mergeui_a_clipped_bottom_layer_is_refused_with_the_reason_and_the_fix) {
    // The subtle one. Merging would leave the clipping base behind and the picture would
    // change, so it declines and says what to do about it.
    pe::app::MainWindow w;
    w.setDocument(
        stackOf({pe::Rgba8{255, 0, 0, 255}, pe::Rgba8{0, 255, 0, 255}, pe::Rgba8{0, 0, 255, 255}}),
        QString());
    auto* doc = w.document();
    doc->topLevelLayers()[1]->setClipped(true);
    doc->setActiveLayer(doc->topLevelLayers()[2]->id());  // merge 2 down into the clipped 1
    w.clearRefusals();
    const std::size_t undoBefore = doc->history().undoDepth();

    PE_REQUIRE(trigger(w, QStringLiteral("Merge Down")));

    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::LayerIsClipped);
    PE_REQUIRE(!w.refusals().empty());
    PE_CHECK(w.refusals().back().explanation.find("Un-clip") != std::string::npos);
    PE_CHECK_EQ(doc->history().undoDepth(), undoBefore);
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(3));
}

PE_TEST(mergeui_merge_visible_takes_the_visible_layers_and_leaves_the_rest) {
    pe::app::MainWindow w;
    w.setDocument(
        stackOf({pe::Rgba8{255, 0, 0, 255}, pe::Rgba8{0, 255, 0, 255}, pe::Rgba8{0, 0, 255, 255}}),
        QString());
    auto* doc = w.document();
    doc->topLevelLayers()[1]->setVisible(false);
    const pe::LayerId hidden = doc->topLevelLayers()[1]->id();

    PE_REQUIRE(trigger(w, QStringLiteral("Merge Visible")));

    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));
    PE_CHECK(doc->findLayer(hidden) != nullptr);  // the hidden layer is untouched
    PE_CHECK(doc->topLevelLayers()[0]->name() == "Merged");
}

PE_TEST(mergeui_merge_visible_with_one_visible_layer_is_refused) {
    pe::app::MainWindow w;
    w.setDocument(stackOf({pe::Rgba8{255, 0, 0, 255}, pe::Rgba8{0, 0, 255, 255}}), QString());
    auto* doc = w.document();
    doc->topLevelLayers()[1]->setVisible(false);
    w.clearRefusals();

    PE_REQUIRE(trigger(w, QStringLiteral("Merge Visible")));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));
}

PE_TEST(mergeui_flatten_takes_everything_and_names_it_background) {
    pe::app::MainWindow w;
    w.setDocument(
        stackOf({pe::Rgba8{255, 0, 0, 255}, pe::Rgba8{0, 255, 0, 255}, pe::Rgba8{0, 0, 255, 255}}),
        QString());
    auto* doc = w.document();
    doc->topLevelLayers()[1]->setVisible(false);

    PE_REQUIRE(trigger(w, QStringLiteral("Flatten Image")));

    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));
    PE_CHECK(doc->topLevelLayers()[0]->name() == "Background");
    // The hidden layer contributed nothing and is gone, which is what flattening means.
    PE_CHECK(pixelOf(doc->topLevelLayers()[0].get(), 8, 8).b > 250);
    PE_CHECK(pixelOf(doc->topLevelLayers()[0].get(), 8, 8).g < 5);
}

PE_TEST(mergeui_flatten_of_a_single_layer_document_is_refused) {
    pe::app::MainWindow w;
    w.setDocument(stackOf({pe::Rgba8{255, 0, 0, 255}}), QString());
    w.clearRefusals();
    PE_REQUIRE(trigger(w, QStringLiteral("Flatten Image")));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
    PE_CHECK_EQ(w.document()->history().undoDepth(), static_cast<std::size_t>(0));
}

PE_TEST(mergeui_export_as_moved_off_the_merge_visible_binding) {
    // Ctrl+Shift+E was Export As. In every editor that has one it is Merge Visible, and Export
    // As is Ctrl+Shift+Alt+W; both are now the conventional binding rather than one of them.
    pe::app::MainWindow w;
    QAction* exportAs = nullptr;
    for (QAction* top : w.menuBar()->actions()) {
        if (top->menu() == nullptr || plain(top->text()) != QStringLiteral("File")) continue;
        for (QAction* a : top->menu()->actions()) {
            if (plain(a->text()).startsWith(QStringLiteral("Export As"))) exportAs = a;
        }
    }
    PE_REQUIRE(exportAs != nullptr);
    PE_CHECK(exportAs->shortcut() == QKeySequence(QStringLiteral("Ctrl+Shift+Alt+W")));
}
