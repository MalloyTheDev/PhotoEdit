// Canvas feedback for gestures that cannot do anything.
//
// A brush press on a layer the paint controller refuses (an adjustment layer, or no
// layer at all) used to be completely silent: nothing changed and nothing was said,
// which looks exactly like a broken brush. Bucket and Gradient already reported it.

#include "CanvasView.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/CanvasRenderer.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <cstddef>
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

// A left-button press, drag and release at explicit widget coordinates. The suite has never
// driven a drag before: every existing case sends a press and nothing else, which is why no
// test could see what a Move or a Transform does between press and release.
void pressAt(pe::app::CanvasView& view, QPointF at) {
    QMouseEvent ev(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton,
                   Qt::NoModifier);
    QCoreApplication::sendEvent(&view, &ev);
}

void moveTo(pe::app::CanvasView& view, QPointF at) {
    // With the button HELD: a drag is the case that matters, and a move with no buttons is
    // delivered differently.
    QMouseEvent ev(QEvent::MouseMove, at, at, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&view, &ev);
}

void releaseAt(pe::app::CanvasView& view, QPointF at) {
    QMouseEvent ev(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton,
                   Qt::NoModifier);
    QCoreApplication::sendEvent(&view, &ev);
}

// Is what the renderer would draw still equal to a composite built from scratch? This is the
// question a bounded invalidation has to keep answering: too small a rect leaves a stale tile
// on screen, and nothing else in the suite would notice.
bool rendererAgreesWithAFreshComposite(pe::app::CanvasView& view, pe::Document& doc) {
    pe::CanvasRenderer* r = view.renderer();
    if (r == nullptr) return false;
    const pe::PixelBuffer live = r->renderRegion(doc.canvasBounds());
    const pe::PixelBuffer fresh = doc.compositeImage();
    if (live.width() != fresh.width() || live.height() != fresh.height()) return false;
    for (int y = 0; y < fresh.height(); ++y) {
        for (int x = 0; x < fresh.width(); ++x) {
            if (!(live.at(x, y) == fresh.at(x, y))) return false;
        }
    }
    return true;
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

PE_TEST(canvasview_a_refused_move_says_why_once) {
    // #180's real complaint was the SILENCE, not the threshold. Raising the limit only moved
    // where the silence starts, so a drag that cannot move anything has to explain itself.
    // Once per drag, not once per motion event, or a slow drag buries the status bar.
    constexpr int T = pe::kTileSize;
    auto doc = pe::Document::createBlank(pe::Size{70 * T, 70 * T});
    PE_REQUIRE(doc != nullptr);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().setPixel(1, 1, pe::Rgba8{1, 2, 3, 255});
    pl->tiles().setPixel(69 * T, 69 * T, pe::Rgba8{4, 5, 6, 255});  // far past the move budget

    pe::app::CanvasView view;
    view.resize(300, 300);
    view.setDocument(doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);

    const QStringList said = messagesFrom(view, [&view] {
        pressAt(view, QPointF(40, 40));
        moveTo(view, QPointF(50, 50));
        moveTo(view, QPointF(60, 60));
        moveTo(view, QPointF(70, 70));
        releaseAt(view, QPointF(70, 70));
    });
    PE_CHECK_EQ(said.size(), 1);  // three motion events, one explanation
    PE_CHECK(!said.isEmpty() && said.first().contains(QStringLiteral("MB")));
    PE_CHECK(!doc->history().canUndo());  // and nothing was committed
    view.setDocument(nullptr);
}

PE_TEST(canvasview_a_move_that_works_says_nothing) {
    // The inverse, so the message above cannot simply always fire.
    auto doc = pe::Document::createBlank(pe::Size{256, 256});
    PE_REQUIRE(doc != nullptr);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{10, 10, 20, 20}, pe::Rgba8{200, 50, 50, 255});

    pe::app::CanvasView view;
    view.resize(300, 300);
    view.setDocument(doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);

    const QStringList said = messagesFrom(view, [&view] {
        pressAt(view, QPointF(20, 20));
        moveTo(view, QPointF(40, 30));
        releaseAt(view, QPointF(40, 30));
    });
    PE_CHECK_EQ(said.size(), 0);
    PE_CHECK(doc->history().canUndo());
    view.setDocument(nullptr);
}

PE_TEST(canvasview_move_drag_keeps_the_tile_cache_instead_of_dropping_it) {
    // Every mouse-move of a drag called reloadImage(), which drops the whole tile LRU and the
    // retained scaled composite, so the next paint recomposited everything visible. The exact
    // dirty rect was already being computed and returned by the preview commands, and thrown
    // away at all four call sites.
    constexpr int T = pe::kTileSize;
    auto doc = pe::Document::createBlank(pe::Size{4 * T, 4 * T});
    PE_REQUIRE(doc != nullptr);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, 24, 24}, pe::Rgba8{200, 50, 50, 255});

    pe::app::CanvasView view;
    view.resize(300, 300);
    view.setDocument(doc.get());
    view.actualPixels();  // 1:1, so widget deltas are document deltas
    view.setTool(pe::app::CanvasView::Tool::Move);
    PE_REQUIRE(view.renderer() != nullptr);

    // Warm the cache over the whole canvas.
    (void)view.renderer()->renderRegion(doc->canvasBounds());
    const std::size_t warm = view.renderer()->cachedTileCount();
    PE_CHECK(warm > 1);  // several tiles, or the assertion below proves nothing

    pressAt(view, QPointF(40, 40));
    moveTo(view, QPointF(44, 44));
    // The content is one tile; a 4 px shift dirties it and its neighbour at most. Dropping
    // the cache would leave nothing at all.
    PE_CHECK(view.renderer()->cachedTileCount() > 1);
    releaseAt(view, QPointF(44, 44));
    view.setDocument(nullptr);
}

PE_TEST(canvasview_move_drag_across_a_tile_boundary_leaves_nothing_stale) {
    // The other direction: a rect that is too SMALL leaves the vacated source showing. The
    // source and destination are in different tiles here, so both halves must be invalidated.
    constexpr int T = pe::kTileSize;
    auto doc = pe::Document::createBlank(pe::Size{4 * T, 2 * T});
    PE_REQUIRE(doc != nullptr);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{10, 10, 20, 20}, pe::Rgba8{200, 50, 50, 255});

    pe::app::CanvasView view;
    view.resize(300, 300);
    view.setDocument(doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);
    (void)view.renderer()->renderRegion(doc->canvasBounds());

    pressAt(view, QPointF(20, 20));
    moveTo(view, QPointF(20 + T + 40, 20));  // well past the first tile boundary
    PE_CHECK(rendererAgreesWithAFreshComposite(view, *doc));
    // Clean again, then a second drag step, so the check below cannot ride on the previous
    // step's outstanding dirty marks.
    moveTo(view, QPointF(20 + T + 80, 30));
    (void)view.renderer()->renderRegion(doc->canvasBounds());
    moveTo(view, QPointF(20 + T + 120, 40));
    PE_CHECK(rendererAgreesWithAFreshComposite(view, *doc));
    releaseAt(view, QPointF(20 + T + 120, 40));
    PE_CHECK(rendererAgreesWithAFreshComposite(view, *doc));
    view.setDocument(nullptr);
}

PE_TEST(canvasview_move_dragged_back_to_the_origin_leaves_nothing_stale) {
    // The sharpest case, and the one a naive bounded implementation gets wrong. Returning to
    // the exact press point makes dx == dy == 0, so moveLayerContent returns null AFTER the
    // previous preview has already been silently reverted. Invalidating only the newly
    // applied command's rect invalidates nothing at all, and the last previewed position
    // stays on screen indefinitely.
    constexpr int T = pe::kTileSize;
    auto doc = pe::Document::createBlank(pe::Size{4 * T, 2 * T});
    PE_REQUIRE(doc != nullptr);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{10, 10, 20, 20}, pe::Rgba8{200, 50, 50, 255});

    pe::app::CanvasView view;
    view.resize(300, 300);
    view.setDocument(doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);
    (void)view.renderer()->renderRegion(doc->canvasBounds());

    pressAt(view, QPointF(20, 20));
    moveTo(view, QPointF(60, 60));  // preview applied
    // Render so the cache is CLEAN and holds the previewed pixels. Without this the dirty
    // marks left by the previous move are still outstanding, the next render recomposites
    // anyway, and the test passes whether or not the revert was invalidated. That is how
    // three mutations survived the first version of this file.
    (void)view.renderer()->renderRegion(doc->canvasBounds());
    moveTo(view, QPointF(20, 20));  // back to the start: the rebuild returns null
    // Only the revert's rect exists now, so this fails outright if it is not invalidated.
    PE_CHECK(rendererAgreesWithAFreshComposite(view, *doc));
    // And the document really is back where it started, so the test is about the cache.
    PE_CHECK_EQ(pl->tiles().pixel(15, 15), (pe::Rgba8{200, 50, 50, 255}));
    releaseAt(view, QPointF(20, 20));
    PE_CHECK(rendererAgreesWithAFreshComposite(view, *doc));
    PE_CHECK(!doc->history().canUndo());  // a zero move commits nothing
    view.setDocument(nullptr);
}

PE_TEST(canvasview_leaving_the_move_tool_mid_drag_leaves_nothing_stale) {
    constexpr int T = pe::kTileSize;
    auto doc = pe::Document::createBlank(pe::Size{4 * T, 2 * T});
    PE_REQUIRE(doc != nullptr);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{10, 10, 20, 20}, pe::Rgba8{200, 50, 50, 255});

    pe::app::CanvasView view;
    view.resize(300, 300);
    view.setDocument(doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);
    (void)view.renderer()->renderRegion(doc->canvasBounds());

    pressAt(view, QPointF(20, 20));
    moveTo(view, QPointF(90, 60));
    (void)view.renderer()->renderRegion(doc->canvasBounds());  // clean, holding the preview
    view.setTool(pe::app::CanvasView::Tool::Brush);            // abandons the preview
    PE_CHECK(rendererAgreesWithAFreshComposite(view, *doc));
    PE_CHECK_EQ(pl->tiles().pixel(15, 15), (pe::Rgba8{200, 50, 50, 255}));  // reverted
    PE_CHECK(!doc->history().canUndo());                                    // and committed nothing
    view.setDocument(nullptr);
}

PE_TEST(canvasview_move_release_commits_one_step_and_leaves_nothing_stale) {
    constexpr int T = pe::kTileSize;
    auto doc = pe::Document::createBlank(pe::Size{4 * T, 2 * T});
    PE_REQUIRE(doc != nullptr);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{10, 10, 20, 20}, pe::Rgba8{200, 50, 50, 255});

    pe::app::CanvasView view;
    view.resize(300, 300);
    view.setDocument(doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);
    (void)view.renderer()->renderRegion(doc->canvasBounds());

    pressAt(view, QPointF(20, 20));
    moveTo(view, QPointF(50, 40));
    moveTo(view, QPointF(70, 55));  // several samples, as a real drag produces
    (void)view.renderer()->renderRegion(doc->canvasBounds());  // clean, holding the preview
    releaseAt(view, QPointF(70, 55));

    PE_CHECK(rendererAgreesWithAFreshComposite(view, *doc));
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));  // exactly one step
    // Moved by the TOTAL, not by the sum of the per-sample deltas.
    PE_CHECK_EQ(pl->tiles().pixel(10 + 50, 10 + 35), (pe::Rgba8{200, 50, 50, 255}));
    PE_CHECK_EQ(pl->tiles().pixel(15, 15), (pe::Rgba8{0, 0, 0, 0}));

    doc->history().undo();
    PE_CHECK(rendererAgreesWithAFreshComposite(view, *doc));
    PE_CHECK_EQ(pl->tiles().pixel(15, 15), (pe::Rgba8{200, 50, 50, 255}));
    view.setDocument(nullptr);
}
