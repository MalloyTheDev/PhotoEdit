// Canvas feedback for gestures that cannot do anything.
//
// A brush press on a layer the paint controller refuses (an adjustment layer, or no
// layer at all) used to be completely silent: nothing changed and nothing was said,
// which looks exactly like a broken brush. Bucket and Gradient already reported it.

#include "CanvasView.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/CanvasRenderer.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QImage>
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

// --- Move tool options (#134) ----------------------------------------------------------

namespace {

// A document with a red square low in the stack, a blue square high in it, and an empty
// active layer at the bottom. Positions are chosen so the two squares do not overlap, so a
// press names one layer unambiguously.
struct MoveScene {
    std::unique_ptr<pe::Document> doc;
    pe::LayerId empty = pe::kNoLayer;
    pe::LayerId low = pe::kNoLayer;
    pe::LayerId high = pe::kNoLayer;
};

MoveScene moveScene() {
    MoveScene s;
    s.doc = pe::Document::createBlank(pe::Size{200, 200});
    if (s.doc == nullptr) return s;
    s.empty = s.doc->activeLayer();

    auto low = std::make_unique<pe::PixelLayer>("Low");
    low->tiles().fillRect(pe::Rect{10, 10, 40, 40}, pe::Rgba8{200, 40, 40, 255});
    s.low = low->id();
    s.doc->cmdInsertTopLevel(s.doc->topLevelCount(), std::move(low));

    auto high = std::make_unique<pe::PixelLayer>("High");
    high->tiles().fillRect(pe::Rect{120, 120, 40, 40}, pe::Rgba8{40, 40, 200, 255});
    s.high = high->id();
    s.doc->cmdInsertTopLevel(s.doc->topLevelCount(), std::move(high));

    s.doc->setActiveLayer(s.empty);
    return s;
}

// Alpha of one document pixel of a raster layer. contentBounds() is TILE aligned, so it
// cannot tell a 30 pixel move from no move at all; the pixels can.
std::uint8_t alphaAt(const pe::Document& doc, pe::LayerId id, int x, int y) {
    const pe::Layer* l = doc.findLayer(id);
    if (l == nullptr || l->kind() != pe::LayerKind::Pixel) return 0;
    return static_cast<const pe::PixelLayer*>(l)->tiles().pixel(x, y).a;
}

void clickDoc(pe::app::CanvasView& view, pe::PointD at) {
    const QPointF w = view.docToWidget(at);
    pressAt(view, w);
    releaseAt(view, w);
}

}  // namespace

PE_TEST(canvasview_auto_select_grabs_the_layer_under_the_cursor) {
    // Without it, a Move drag always moves whatever is active, so compositing means going
    // back to the Layers panel between every drag. The option is off by default, which is
    // the behaviour that shipped, and the OFF case is asserted rather than assumed.
    MoveScene s = moveScene();
    PE_REQUIRE(s.doc != nullptr);

    pe::app::CanvasView view;
    view.resize(400, 400);
    view.setDocument(s.doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);

    PE_CHECK(!view.autoSelect());
    clickDoc(view, pe::PointD{30, 30});
    PE_CHECK(s.doc->activeLayer() == s.empty);  // off: the click selects nothing

    view.setAutoSelect(true);
    clickDoc(view, pe::PointD{30, 30});
    PE_CHECK(s.doc->activeLayer() == s.low);
    clickDoc(view, pe::PointD{140, 140});
    PE_CHECK(s.doc->activeLayer() == s.high);

    view.setDocument(nullptr);
}

PE_TEST(canvasview_auto_select_moves_the_layer_it_picked) {
    // Picking the layer is only half of it: the drag that follows has to move THAT layer,
    // not the one that was active when the press arrived.
    MoveScene s = moveScene();
    PE_REQUIRE(s.doc != nullptr);
    // The square spans x in [10, 50). After a 30 pixel move right it spans [40, 80), so
    // these two points swap.
    PE_REQUIRE(alphaAt(*s.doc, s.low, 15, 30) == 255);
    PE_REQUIRE(alphaAt(*s.doc, s.low, 75, 30) == 0);

    pe::app::CanvasView view;
    view.resize(400, 400);
    view.setDocument(s.doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);
    view.setAutoSelect(true);

    pressAt(view, view.docToWidget(pe::PointD{30, 30}));
    moveTo(view, view.docToWidget(pe::PointD{60, 30}));
    releaseAt(view, view.docToWidget(pe::PointD{60, 30}));

    PE_CHECK(s.doc->activeLayer() == s.low);
    PE_CHECK_EQ(s.doc->history().undoDepth(), static_cast<std::size_t>(1));
    PE_CHECK_EQ(alphaAt(*s.doc, s.low, 15, 30), 0);
    PE_CHECK_EQ(alphaAt(*s.doc, s.low, 75, 30), 255);
    // And the layer that was active when the press arrived is untouched.
    PE_CHECK(s.doc->findLayer(s.empty)->contentBounds().isEmpty());

    view.setDocument(nullptr);
}

PE_TEST(canvasview_auto_select_on_empty_canvas_starts_no_drag) {
    // Falling back to the active layer here is exactly the surprise Auto-Select exists to
    // remove: the user aimed at nothing and would have dragged something else. Refusing the
    // drag is right, and saying so is what stops it reading as a broken tool.
    MoveScene s = moveScene();
    PE_REQUIRE(s.doc != nullptr);

    pe::app::CanvasView view;
    view.resize(400, 400);
    view.setDocument(s.doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);
    view.setAutoSelect(true);
    s.doc->setActiveLayer(s.low);

    const QStringList said = messagesFrom(view, [&view] {
        pressAt(view, view.docToWidget(pe::PointD{100, 20}));  // between the two squares
        moveTo(view, view.docToWidget(pe::PointD{140, 20}));
        releaseAt(view, view.docToWidget(pe::PointD{140, 20}));
    });
    PE_CHECK_EQ(said.size(), 1);
    PE_CHECK(!said.isEmpty() && said.first().contains(QStringLiteral("Auto-Select")));
    // Nothing selected, nothing moved, nothing on the undo stack.
    PE_CHECK(s.doc->activeLayer() == s.low);
    PE_CHECK_EQ(s.doc->history().undoDepth(), static_cast<std::size_t>(0));
    PE_CHECK_EQ(alphaAt(*s.doc, s.low, 15, 30), 255);  // still exactly where it was

    view.setDocument(nullptr);
}

PE_TEST(canvasview_auto_select_group_mode_picks_the_top_level_group) {
    // The granularity combo has to change the ANSWER, or it is another decorative control.
    auto doc = pe::Document::createBlank(pe::Size{200, 200});
    PE_REQUIRE(doc != nullptr);
    const pe::LayerId base = doc->activeLayer();
    auto group = std::make_unique<pe::GroupLayer>("G");
    auto inner = std::make_unique<pe::PixelLayer>("Inner");
    inner->tiles().fillRect(pe::Rect{10, 10, 40, 40}, pe::Rgba8{200, 40, 40, 255});
    const pe::LayerId innerId = inner->id();
    group->addChild(std::move(inner));
    const pe::LayerId groupId = group->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));
    doc->setActiveLayer(base);

    pe::app::CanvasView view;
    view.resize(400, 400);
    view.setDocument(doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);
    view.setAutoSelect(true);

    PE_CHECK(view.autoSelectMode() == pe::app::CanvasView::AutoSelectMode::Layer);
    clickDoc(view, pe::PointD{30, 30});
    PE_CHECK(doc->activeLayer() == innerId);

    doc->setActiveLayer(base);
    view.setAutoSelectMode(pe::app::CanvasView::AutoSelectMode::Group);
    clickDoc(view, pe::PointD{30, 30});
    PE_CHECK(doc->activeLayer() == groupId);

    view.setDocument(nullptr);
}

PE_TEST(canvasview_show_transform_controls_starts_a_transform_from_a_handle) {
    // The box is not decoration: grabbing a corner is meant to start a Free Transform
    // without Ctrl+T, and the canvas has to tell the shell that the tool changed or the
    // strip keeps claiming Move.
    MoveScene s = moveScene();
    PE_REQUIRE(s.doc != nullptr);
    s.doc->setActiveLayer(s.low);

    pe::app::CanvasView view;
    view.resize(400, 400);
    view.setDocument(s.doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);

    std::vector<pe::app::CanvasView::Tool> announced;
    QObject::connect(&view, &pe::app::CanvasView::toolChanged,
                     [&announced](pe::app::CanvasView::Tool t) { announced.push_back(t); });

    // Off: the corner is just canvas, so the press starts a move.
    PE_CHECK(!view.showTransformControls());
    const pe::Rect box = s.doc->findLayer(s.low)->contentBounds();
    const QPointF corner =
        view.docToWidget(pe::PointD{static_cast<double>(box.x), static_cast<double>(box.y)});
    pressAt(view, corner);
    releaseAt(view, corner);
    PE_CHECK(view.activeTool() == pe::app::CanvasView::Tool::Move);
    PE_CHECK(announced.empty());

    // On: the same press enters Free Transform and says so.
    view.setShowTransformControls(true);
    pressAt(view, corner);
    PE_CHECK(view.activeTool() == pe::app::CanvasView::Tool::Transform);
    PE_REQUIRE(announced.size() == 1);
    PE_CHECK(announced[0] == pe::app::CanvasView::Tool::Transform);

    // Dragging that handle scales the layer, and the release commits one undo step.
    moveTo(view, view.docToWidget(pe::PointD{static_cast<double>(box.x) - 20.0,
                                             static_cast<double>(box.y) - 20.0}));
    releaseAt(view, view.docToWidget(pe::PointD{static_cast<double>(box.x) - 20.0,
                                                static_cast<double>(box.y) - 20.0}));
    view.setTool(pe::app::CanvasView::Tool::Move);  // leaving Free Transform commits it
    PE_CHECK(s.doc->history().undoDepth() >= static_cast<std::size_t>(1));

    view.setDocument(nullptr);
}

PE_TEST(canvasview_a_press_inside_the_transform_box_is_still_a_move) {
    // Only the handles start a transform. A press in the middle of the box has to keep
    // doing what the Move tool does, or turning the controls on would take the tool away.
    MoveScene s = moveScene();
    PE_REQUIRE(s.doc != nullptr);
    s.doc->setActiveLayer(s.low);
    // The square spans x in [10, 50); a 25 pixel move right puts it at [35, 75).
    PE_REQUIRE(alphaAt(*s.doc, s.low, 15, 30) == 255);
    PE_REQUIRE(alphaAt(*s.doc, s.low, 70, 30) == 0);

    pe::app::CanvasView view;
    view.resize(400, 400);
    view.setDocument(s.doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Move);
    view.setShowTransformControls(true);

    pressAt(view, view.docToWidget(pe::PointD{30, 30}));  // well inside the box
    moveTo(view, view.docToWidget(pe::PointD{55, 30}));
    releaseAt(view, view.docToWidget(pe::PointD{55, 30}));

    PE_CHECK(view.activeTool() == pe::app::CanvasView::Tool::Move);
    PE_CHECK_EQ(alphaAt(*s.doc, s.low, 15, 30), 0);
    PE_CHECK_EQ(alphaAt(*s.doc, s.low, 70, 30), 255);

    view.setDocument(nullptr);
}

PE_TEST(canvasview_a_transform_drag_only_resamples_what_is_on_screen) {
    // Free Transform rebuilt the whole layer on every motion event, which on a 16 megapixel
    // document was about a second per mouse-move: not a slow tool but an unusable one. The
    // preview is bounded to the viewport, so the cost follows the window rather than the
    // document. What must stay true is that the commit is NOT bounded: an off-screen pixel
    // keeps its original value during the drag and is transformed when the drag ends.
    constexpr int kDim = 1200;
    auto doc = pe::Document::createBlank(pe::Size{kDim, kDim});
    PE_REQUIRE(doc != nullptr);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    for (int y = 0; y < kDim; ++y) {
        pl->tiles().fillRect(pe::Rect{0, y, kDim, 1},
                             pe::Rgba8{static_cast<std::uint8_t>(y % 251), 90, 200, 255});
    }
    pe::app::CanvasView view;
    view.resize(300, 300);
    view.setDocument(doc.get());
    view.actualPixels();  // 100% zoom, so most of the document is off screen

    const pe::Rect visible = view.visibleDocRect();
    PE_REQUIRE(!visible.isEmpty());
    PE_REQUIRE(visible.width < kDim);  // the point of the case: the view shows only part

    // A document pixel well outside the viewport, and one inside it.
    const pe::Point off{kDim - 40, kDim - 40};
    const pe::Point on{visible.x + visible.width / 2, visible.y + visible.height / 2};
    PE_REQUIRE(!visible.contains(off));
    const pe::Rgba8 offBefore = pl->tiles().pixel(off.x, off.y);
    const pe::Rgba8 onBefore = pl->tiles().pixel(on.x, on.y);

    // setTool begins the session, so the top-left corner handle is live straight away.
    view.setTool(pe::app::CanvasView::Tool::Transform);
    const QPointF corner = view.docToWidget(pe::PointD{0.0, 0.0});
    pressAt(view, corner);
    moveTo(view, corner + QPointF(-40.0, -40.0));

    // Mid-drag: the visible pixel is being previewed, the off-screen one is not touched.
    PE_CHECK(pl->tiles().pixel(on.x, on.y) != onBefore);
    PE_CHECK(pl->tiles().pixel(off.x, off.y) == offBefore);

    releaseAt(view, corner + QPointF(-40.0, -40.0));
    view.setTool(pe::app::CanvasView::Tool::Move);  // leaving Free Transform commits it

    // Committed: the off-screen pixel has moved too, and it is one undo step.
    PE_CHECK(pl->tiles().pixel(off.x, off.y) != offBefore);
    PE_CHECK(doc->history().undoDepth() >= static_cast<std::size_t>(1));

    view.setDocument(nullptr);
}

PE_TEST(canvasview_the_visible_rect_covers_the_widget_and_is_padded) {
    // What bounds the preview. Too small and the drag tears at the window edge; unpadded and
    // a bilinear tap at the boundary reads a neighbour that was never resampled.
    auto doc = pe::Document::createBlank(pe::Size{2000, 2000});
    PE_REQUIRE(doc != nullptr);
    pe::app::CanvasView view;
    view.resize(400, 300);
    view.setDocument(doc.get());
    view.actualPixels();

    const pe::Rect r = view.visibleDocRect();
    PE_REQUIRE(!r.isEmpty());
    // At 100% zoom it is the widget plus a little padding on each side, and no more: a rect
    // that grew with the document would defeat the whole point.
    PE_CHECK(r.width >= 400 + 8 && r.width <= 400 + 16);
    PE_CHECK(r.height >= 300 + 8 && r.height <= 300 + 16);
    view.setDocument(nullptr);
}

PE_TEST(canvasview_a_lasso_leaves_ants_that_follow_the_shape_it_drew) {
    // The user-visible defect: the freehand path drew correctly while the mouse was down and
    // became a rectangle the moment it came up, because the committed selection's overlay
    // was tightBounds(). The mask was right all along, which is why parts of that rectangle
    // then would not paint.
    auto doc = pe::Document::createBlank(pe::Size{400, 400});
    PE_REQUIRE(doc != nullptr);

    pe::app::CanvasView view;
    view.resize(500, 500);
    view.setDocument(doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Lasso);

    // A triangle, whose boundary no rectangle can describe.
    pressAt(view, view.docToWidget(pe::PointD{40.0, 40.0}));
    moveTo(view, view.docToWidget(pe::PointD{240.0, 40.0}));
    moveTo(view, view.docToWidget(pe::PointD{40.0, 240.0}));
    releaseAt(view, view.docToWidget(pe::PointD{40.0, 240.0}));

    PE_REQUIRE(doc->selection().active());
    const QVector<QLineF>& ants = view.selectionOutline();
    PE_REQUIRE(!ants.isEmpty());
    // Four segments is a rectangle. A triangle's staircase hypotenuse is many more.
    PE_CHECK(ants.size() > 4);

    // And the ants agree with what actually paints: the corner outside the triangle but
    // inside its bounding box is not selected, and no ant segment claims it is.
    const pe::Rect b = doc->selection().tightBounds();
    PE_CHECK_EQ(doc->selection().value(b.right() - 2, b.bottom() - 2), static_cast<uint8_t>(0));
    bool touchesBoxCorner = false;
    for (const QLineF& l : ants) {
        if (l.x1() >= b.right() && l.y1() >= b.bottom()) touchesBoxCorner = true;
        if (l.x2() >= b.right() && l.y2() >= b.bottom()) touchesBoxCorner = true;
    }
    PE_CHECK(!touchesBoxCorner);

    view.setDocument(nullptr);
}

PE_TEST(canvasview_a_rectangular_marquee_still_leaves_four_ant_segments) {
    // The inverse, so the fix cannot have been "always draw many segments": a rectangle's
    // outline is still four merged edges, not one per pixel.
    auto doc = pe::Document::createBlank(pe::Size{400, 400});
    PE_REQUIRE(doc != nullptr);

    pe::app::CanvasView view;
    view.resize(500, 500);
    view.setDocument(doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Marquee);

    pressAt(view, view.docToWidget(pe::PointD{40.0, 40.0}));
    moveTo(view, view.docToWidget(pe::PointD{240.0, 140.0}));
    releaseAt(view, view.docToWidget(pe::PointD{240.0, 140.0}));

    PE_REQUIRE(doc->selection().active());
    PE_CHECK_EQ(view.selectionOutline().size(), 4);
    view.setDocument(nullptr);
}

PE_TEST(canvasview_the_ants_are_painted_along_the_shape_not_its_box) {
    // The one that matches what the user saw. The cached outline being right is not the same
    // as the overlay being drawn from it, and the defect was entirely in the drawing: the
    // committed selection was painted as drawAntRect(tightBounds()).
    //
    // Compares two renders of the same view, so the difference between them IS the overlay,
    // whatever colour the dashes happen to land on.
    auto doc = pe::Document::createBlank(pe::Size{400, 400});
    PE_REQUIRE(doc != nullptr);

    pe::app::CanvasView view;
    view.resize(500, 500);
    view.setDocument(doc.get());
    view.actualPixels();
    view.setTool(pe::app::CanvasView::Tool::Lasso);
    const QImage before = view.grab().toImage();
    PE_REQUIRE(!before.isNull());

    // A right triangle: (40,40), (240,40), (40,240). Its hypotenuse runs through (140,140),
    // and the right edge of its bounding box at (240,140) is nowhere near the selection.
    pressAt(view, view.docToWidget(pe::PointD{40.0, 40.0}));
    moveTo(view, view.docToWidget(pe::PointD{240.0, 40.0}));
    moveTo(view, view.docToWidget(pe::PointD{40.0, 240.0}));
    releaseAt(view, view.docToWidget(pe::PointD{40.0, 240.0}));
    PE_REQUIRE(doc->selection().active());

    const QImage after = view.grab().toImage();
    PE_REQUIRE(after.size() == before.size());

    const auto changedNear = [&](pe::PointD at, int radius) {
        const QPointF w = view.docToWidget(at);
        const int cx = static_cast<int>(std::lround(w.x()));
        const int cy = static_cast<int>(std::lround(w.y()));
        for (int y = cy - radius; y <= cy + radius; ++y) {
            for (int x = cx - radius; x <= cx + radius; ++x) {
                if (x < 0 || y < 0 || x >= before.width() || y >= before.height()) continue;
                if (before.pixel(x, y) != after.pixel(x, y)) return true;
            }
        }
        return false;
    };

    PE_CHECK(changedNear(pe::PointD{140.0, 140.0}, 4));   // on the hypotenuse: ants
    PE_CHECK(!changedNear(pe::PointD{240.0, 140.0}, 4));  // on the box's right edge: none
    PE_CHECK(!changedNear(pe::PointD{200.0, 200.0}, 4));  // and the box's far corner: none

    view.setDocument(nullptr);
}

PE_TEST(canvasview_says_so_when_a_selection_outline_is_too_detailed_to_draw) {
    // A boundary ragged enough to have more segments than are worth drawing every frame is
    // given up on. That has to be SAID: falling back to the bounding box without a word is
    // the original defect wearing a hat, and the user would again be looking at a rectangle
    // that paints like something else.
    auto doc = pe::Document::createBlank(pe::Size{900, 900});
    PE_REQUIRE(doc != nullptr);

    // Noise, not a checkerboard. A checkerboard looks maximally ragged and is not: every row
    // differs from the one above it at every pixel, so the horizontal boundary merges into
    // ONE run per row and the whole thing traces to a few hundred segments. Irregularity is
    // what defeats merging, so the mask is a deterministic pseudo-random one.
    constexpr int kSide = 800;
    pe::PixelBuffer mask(kSide, kSide, pe::Rgba8{0, 0, 0, 255});
    std::uint32_t seed = 12345u;
    for (int y = 0; y < kSide; ++y) {
        for (int x = 0; x < kSide; ++x) {
            seed = seed * 1664525u + 1013904223u;  // a plain LCG: reproducible across runs
            if ((seed >> 16) & 1u) mask.set(x, y, pe::Rgba8{255, 255, 255, 255});
        }
    }
    const auto ragged = [&mask] {
        pe::Selection s;
        s.loadMask(mask, 20, 20);
        return s;
    };

    pe::app::CanvasView view;
    view.resize(300, 300);
    view.setDocument(doc.get());

    QStringList said = messagesFrom(
        view, [&] { doc->history().push(std::make_unique<pe::SetSelectionCommand>(ragged())); });
    PE_CHECK_EQ(said.size(), 1);
    PE_CHECK(!said.isEmpty() && said.first().contains(QStringLiteral("bounds")));
    PE_CHECK(view.selectionOutline().isEmpty());  // nothing, rather than half an outline

    // A second untraceable selection does not repeat itself: the state has not changed.
    said = messagesFrom(
        view, [&] { doc->history().push(std::make_unique<pe::SetSelectionCommand>(ragged())); });
    PE_CHECK_EQ(said.size(), 0);

    // But once a traceable selection has been shown, an untraceable one is worth saying again.
    said = messagesFrom(view, [&] {
        pe::Selection simple;
        simple.selectRect(pe::Rect{10, 10, 50, 50});
        doc->history().push(std::make_unique<pe::SetSelectionCommand>(std::move(simple)));
    });
    PE_CHECK_EQ(said.size(), 0);
    PE_CHECK_EQ(view.selectionOutline().size(), 4);

    said = messagesFrom(
        view, [&] { doc->history().push(std::make_unique<pe::SetSelectionCommand>(ragged())); });
    PE_CHECK_EQ(said.size(), 1);

    view.setDocument(nullptr);
}
