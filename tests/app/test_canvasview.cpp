// Canvas feedback for gestures that cannot do anything.
//
// A brush press on a layer the paint controller refuses (an adjustment layer, or no
// layer at all) used to be completely silent: nothing changed and nothing was said,
// which looks exactly like a broken brush. Bucket and Gradient already reported it.

#include "CanvasView.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <memory>
#include <utility>

#include <QCoreApplication>
#include <QMouseEvent>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>

namespace {

// Every toolMessage the view emits while `body` runs.
template <typename F>
QStringList messagesFrom(pe::app::CanvasView& view, F body) {
    QStringList seen;
    const QMetaObject::Connection c = QObject::connect(&view, &pe::app::CanvasView::toolMessage,
                                                       [&seen](const QString& m) { seen << m; });
    body();
    QObject::disconnect(c);
    return seen;
}

// A left-button press at the centre of the (unshown, default-sized) view.
void pressLeft(pe::app::CanvasView& view) {
    const QPointF at(static_cast<qreal>(view.width()) / 2.0,
                     static_cast<qreal>(view.height()) / 2.0);
    QMouseEvent ev(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton,
                   Qt::NoModifier);
    QCoreApplication::sendEvent(&view, &ev);
}

}  // namespace

PE_TEST(canvasview_brush_on_an_unpaintable_layer_says_so) {
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_CHECK(doc != nullptr);

    // An adjustment layer is not paintable pixels, so the stroke cannot begin.
    auto adj = std::make_unique<pe::AdjustmentLayer>(std::make_unique<pe::Invert>(), "Invert");
    const pe::LayerId adjId = adj->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(adj));
    doc->setActiveLayer(adjId);

    pe::app::CanvasView view;
    view.resize(200, 200);
    view.setDocument(doc.get());
    view.setTool(pe::app::CanvasView::Tool::Brush);

    const QStringList said = messagesFrom(view, [&] { pressLeft(view); });
    PE_CHECK_EQ(said.size(), 1);
    PE_CHECK(said.value(0).contains(QStringLiteral("pixel layer")));

    view.setDocument(nullptr);
}

PE_TEST(canvasview_brush_with_no_active_layer_says_so) {
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_CHECK(doc != nullptr);
    doc->setActiveLayer(pe::kNoLayer);

    pe::app::CanvasView view;
    view.resize(200, 200);
    view.setDocument(doc.get());
    view.setTool(pe::app::CanvasView::Tool::Brush);

    const QStringList said = messagesFrom(view, [&] { pressLeft(view); });
    PE_CHECK_EQ(said.size(), 1);
    PE_CHECK(said.value(0).contains(QStringLiteral("layer")));

    view.setDocument(nullptr);
}

PE_TEST(canvasview_brush_on_a_pixel_layer_stays_quiet) {
    // The guard against a message on every stroke: a press that really paints must
    // say nothing at all, or the status bar would nag through normal use.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_CHECK(doc != nullptr);

    pe::app::CanvasView view;
    view.resize(200, 200);
    view.setDocument(doc.get());
    view.setTool(pe::app::CanvasView::Tool::Brush);

    const QStringList said = messagesFrom(view, [&] { pressLeft(view); });
    PE_CHECK_EQ(said.size(), 0);
    PE_CHECK(view.tool().isStroking());  // the stroke really did begin

    view.tool().cancel(*doc);
    view.setDocument(nullptr);
}

PE_TEST(canvasview_wand_click_selects_what_a_flattened_composite_would) {
    // The wand no longer flattens through Document::compositeImage(); it samples the
    // renderer's tile cache instead, and the click runs on a worker thread. Neither may
    // change WHAT gets selected, so the reference here is the old path's own answer.
    //
    // Several tiles across, and the seed's region spans all of them, so a per-tile mistake
    // in the cached path cannot hide inside one tile.
    auto doc = pe::Document::createBlank(pe::Size{2 * pe::kTileSize, 2 * pe::kTileSize});
    PE_CHECK(doc != nullptr);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, 2 * pe::kTileSize, 2 * pe::kTileSize},
                         pe::Rgba8{40, 80, 220, 255});
    // A patch of a different colour, deliberately away from the centre so the seed lands
    // in the large region and the selection has to cross every tile boundary.
    pl->tiles().fillRect(pe::Rect{0, 0, 60, 60}, pe::Rgba8{240, 40, 40, 255});

    pe::app::CanvasView view;
    view.resize(300, 220);
    view.setDocument(doc.get());
    view.setTool(pe::app::CanvasView::Tool::Wand);
    view.actualPixels();  // 100% zoom, centred: the widget centre is the canvas centre

    const pe::Selection expected =
        pe::magicWandSelection(doc->compositeImage(), pe::kTileSize, pe::kTileSize, 32);
    PE_CHECK(expected.active());

    const QStringList said = messagesFrom(view, [&] { pressLeft(view); });
    PE_CHECK_EQ(said.size(), 0);  // a successful selection says nothing
    PE_CHECK(doc->selection().active());
    PE_CHECK(doc->selection() == expected);
    PE_CHECK(doc->history().canUndo());  // and it went through history, so it is undoable

    // The selection really does span more than one tile, or the case above proves little.
    PE_CHECK(doc->selection().tightBounds().width > pe::kTileSize);
    PE_CHECK(doc->selection().tightBounds().height > pe::kTileSize);

    view.setDocument(nullptr);
}

PE_TEST(canvasview_wand_leaves_the_canvas_thawed_after_a_click) {
    // The click freezes the canvas while the worker samples the document. If a return path
    // ever skipped the thaw, the canvas would sit showing a still image for the rest of the
    // session, which looks exactly like a hung renderer.
    auto doc = pe::Document::createBlank(pe::Size{2 * pe::kTileSize, 2 * pe::kTileSize});
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, 2 * pe::kTileSize, 2 * pe::kTileSize},
                         pe::Rgba8{40, 80, 220, 255});

    pe::app::CanvasView view;
    view.resize(300, 220);
    view.setDocument(doc.get());
    view.setTool(pe::app::CanvasView::Tool::Wand);
    view.actualPixels();
    pressLeft(view);
    PE_CHECK(!view.isFrozen());

    // A click on empty canvas takes the other exit, and must thaw too.
    auto blank = pe::Document::createBlank(pe::Size{2 * pe::kTileSize, 2 * pe::kTileSize});
    view.setDocument(blank.get());
    view.actualPixels();
    pressLeft(view);
    PE_CHECK(!view.isFrozen());

    view.setDocument(nullptr);
}
