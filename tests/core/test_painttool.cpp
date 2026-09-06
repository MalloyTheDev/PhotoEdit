#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PaintToolController.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <algorithm>
#include <memory>
#include <vector>

using namespace pe;

namespace {
constexpr Rgbaf kRedF{1.0f, 0.0f, 0.0f, 1.0f};

int alphaAt(const Document& doc, LayerId id, int x, int y) {
    const auto* pl = static_cast<const PixelLayer*>(doc.findLayer(id));
    return pl->tiles().pixel(x, y).a;
}

BrushSettings hardBrush(float diameter) {
    BrushSettings b;
    b.diameter = diameter;
    b.hardness = 1.0f;
    b.opacity = 1.0f;
    b.flow = 1.0f;
    b.spacing = 0.25f;
    return b;
}

StrokePoint pt(float x, float y) {
    return StrokePoint{{x, y}, 1.0f};
}
}  // namespace

PE_TEST(painttool_single_click_paints_one_undo_step) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();

    PaintToolController tool;
    tool.setBrush(hardBrush(16));
    tool.setColor(kRedF);

    PE_CHECK(tool.begin(*doc, pt(32, 32)));
    PE_CHECK(tool.isStroking());
    PE_CHECK(tool.end(*doc));
    PE_CHECK(!tool.isStroking());

    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);  // dab deposited
    PE_CHECK_EQ(doc->history().undoDepth(), 1u);    // exactly one undo step
    PE_CHECK_EQ(doc->history().topUndoName(), std::string("Brush"));
    PE_CHECK(doc->isDirty());

    doc->history().undo();
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 0);  // restored transparent
    doc->history().redo();
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);  // repainted
}

PE_TEST(painttool_multisample_stroke_is_single_undo_step) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();

    PaintToolController tool;
    tool.setBrush(hardBrush(12));
    tool.setColor(kRedF);

    PE_CHECK(tool.begin(*doc, pt(8, 32)));
    tool.extend(*doc, pt(24, 32));
    tool.extend(*doc, pt(40, 32));
    tool.extend(*doc, pt(56, 32));
    PE_CHECK(tool.end(*doc));

    // The whole path is painted, yet it collapses to ONE undo step.
    PE_CHECK_EQ(alphaAt(*doc, base, 8, 32), 255);
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);
    PE_CHECK_EQ(alphaAt(*doc, base, 56, 32), 255);
    PE_CHECK_EQ(doc->history().undoDepth(), 1u);

    doc->history().undo();  // one undo erases the entire stroke
    PE_CHECK_EQ(alphaAt(*doc, base, 8, 32), 0);
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 0);
    PE_CHECK_EQ(alphaAt(*doc, base, 56, 32), 0);
}

PE_TEST(painttool_live_stroke_dropped_when_target_layer_removed_midstroke) {
    // Contract violation: a layer is removed mid-stroke. The incremental live stroke borrows that
    // layer's tile store by reference, so the controller must drop the stroke rather than touch the
    // freed store (run under ASan to actually catch a dangling-store dereference). A second layer
    // keeps the document valid after the removal.
    auto doc = Document::createBlank(Size{64, 64});
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::make_unique<PixelLayer>("Other"));
    const LayerId target = doc->activeLayer();  // the base layer the stroke paints

    PaintToolController tool;
    tool.setBrush(hardBrush(12));
    tool.setColor(kRedF);

    PE_CHECK(tool.begin(*doc, pt(8, 32)));
    tool.extend(*doc, pt(24, 32));  // a live dab lands on the target's store
    PE_CHECK(tool.isStroking());

    {
        auto removed =
            doc->cmdRemoveTopLevel(target);  // drop the unique_ptr -> layer + store freed
        PE_CHECK(removed != nullptr);
    }
    PE_CHECK(doc->findLayer(target) == nullptr);

    tool.extend(*doc, pt(40, 32));  // guard fires: drop without dereferencing the freed store
    PE_CHECK(!tool.isStroking());   // stroke abandoned
    PE_CHECK(!tool.end(*doc));      // nothing committed
    PE_CHECK_EQ(doc->history().undoDepth(), 0u);
}

PE_TEST(painttool_preview_visible_before_commit) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();

    PaintToolController tool;
    tool.setBrush(hardBrush(12));
    tool.setColor(kRedF);

    tool.begin(*doc, pt(8, 32));
    tool.extend(*doc, pt(56, 32));
    // Mid-stroke: pixels show the live preview, but NOTHING is in history yet.
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);
    PE_CHECK_EQ(doc->history().undoDepth(), 0u);

    tool.end(*doc);
    PE_CHECK_EQ(doc->history().undoDepth(), 1u);
}

PE_TEST(painttool_cancel_reverts_and_commits_nothing) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();

    PaintToolController tool;
    tool.setBrush(hardBrush(12));
    tool.setColor(kRedF);

    tool.begin(*doc, pt(8, 32));
    tool.extend(*doc, pt(56, 32));
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);  // preview applied

    tool.cancel(*doc);
    PE_CHECK(!tool.isStroking());
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 0);  // preview reverted
    PE_CHECK_EQ(doc->history().undoDepth(), 0u);  // nothing committed
    PE_CHECK(!doc->isDirty());
}

PE_TEST(painttool_result_equals_single_paintstroke) {
    // The interactive begin/extend/end path must produce byte-identical pixels to a
    // one-shot paintStroke over the same points (the live-preview dance is exact).
    const std::vector<StrokePoint> pts = {pt(8, 8), pt(40, 24), pt(20, 50), pt(56, 56)};

    auto viaTool = Document::createBlank(Size{64, 64});
    PaintToolController tool;
    tool.setBrush(hardBrush(14));
    tool.setColor(kRedF);
    tool.begin(*viaTool, pts[0]);
    for (std::size_t i = 1; i < pts.size(); ++i) tool.extend(*viaTool, pts[i]);
    tool.end(*viaTool);

    auto viaOneShot = Document::createBlank(Size{64, 64});
    auto cmd = paintStroke(*viaOneShot, viaOneShot->activeLayer(), hardBrush(14), kRedF, pts);
    PE_CHECK(cmd != nullptr);
    viaOneShot->history().push(std::move(cmd));

    const PixelBuffer a = viaTool->compositeImage();
    const PixelBuffer b = viaOneShot->compositeImage();
    PE_CHECK_EQ(a.width(), b.width());
    PE_CHECK_EQ(a.height(), b.height());
    bool identical = a.width() == b.width() && a.height() == b.height();
    if (identical) {
        const std::size_t count =
            static_cast<std::size_t>(a.width()) * static_cast<std::size_t>(a.height());
        for (std::size_t i = 0; i < count; ++i) {
            if (!(a.data()[i] == b.data()[i])) {
                identical = false;
                break;
            }
        }
    }
    PE_CHECK(identical);
}

PE_TEST(painttool_eraser_reduces_alpha) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();

    PaintToolController tool;
    tool.setBrush(hardBrush(24));
    tool.setColor(kRedF);
    tool.begin(*doc, pt(32, 32));
    tool.end(*doc);
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);

    tool.setMode(PaintToolController::Mode::Eraser);
    tool.begin(*doc, pt(32, 32));
    tool.end(*doc);
    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 0);  // erased back to transparent
    PE_CHECK_EQ(doc->history().undoDepth(), 2u);  // paint + erase, each one step
    PE_CHECK_EQ(doc->history().topUndoName(), std::string("Eraser"));
}

PE_TEST(painttool_maskpaint_paints_mask_not_pixels) {
    // MaskPaint routes the brush into the active layer's MASK: the layer's pixels are untouched,
    // but the composite hides where the (black) brush painted. One undo step named "Mask Brush".
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 64, 64}, Rgba8{255, 0, 0, 255});
    pl->setMask(std::make_unique<Mask>(Mask::Kind::Layer));  // reveal-all mask

    PaintToolController tool;
    tool.setBrush(hardBrush(16));
    tool.setColor(Rgbaf{0.0f, 0.0f, 0.0f, 1.0f});  // black foreground -> hide
    tool.setMode(PaintToolController::Mode::MaskPaint);

    PE_CHECK(tool.begin(*doc, pt(32, 32)));
    PE_CHECK(tool.end(*doc));

    PE_CHECK_EQ(alphaAt(*doc, base, 32, 32), 255);  // the layer's PIXELS are untouched
    PE_CHECK(static_cast<int>(doc->compositeImage().at(32, 32).a) < 40);   // composite hidden there
    PE_CHECK_EQ(static_cast<int>(doc->compositeImage().at(2, 2).a), 255);  // revealed elsewhere
    PE_CHECK_EQ(doc->history().undoDepth(), 1u);
    PE_CHECK_EQ(doc->history().topUndoName(), std::string("Mask Brush"));

    doc->history().undo();
    PE_CHECK_EQ(static_cast<int>(doc->compositeImage().at(32, 32).a), 255);  // reveal restored
}

PE_TEST(painttool_maskpaint_white_reveals) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(base));
    pl->tiles().fillRect(Rect{0, 0, 64, 64}, Rgba8{0, 0, 255, 255});
    auto mask = std::make_unique<Mask>(Mask::Kind::Layer);
    mask->buffer().fillRect(Rect{0, 0, 64, 64}, MaskBuffer::kClear);  // hide all
    pl->setMask(std::move(mask));

    PaintToolController tool;
    tool.setBrush(hardBrush(16));
    tool.setColor(Rgbaf{1.0f, 1.0f, 1.0f, 1.0f});  // white foreground -> reveal
    tool.setMode(PaintToolController::Mode::MaskPaint);
    PE_CHECK(tool.begin(*doc, pt(32, 32)));
    PE_CHECK(tool.end(*doc));

    PE_CHECK(static_cast<int>(doc->compositeImage().at(32, 32).a) > 200);  // revealed where painted
    PE_CHECK_EQ(static_cast<int>(doc->compositeImage().at(2, 2).a), 0);    // still hidden elsewhere
}

PE_TEST(painttool_maskpaint_refuses_without_mask) {
    // MaskPaint needs a mask on the active layer; a pixel layer with none can't begin a mask
    // stroke.
    auto doc = Document::createBlank(Size{64, 64});
    PaintToolController tool;
    tool.setMode(PaintToolController::Mode::MaskPaint);
    PE_CHECK(!tool.begin(*doc, pt(32, 32)));
    PE_CHECK(!tool.isStroking());
}

PE_TEST(painttool_selection_gates_stroke) {
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();

    Selection sel;
    sel.selectRect(Rect{0, 0, 32, 64});  // only the left half is editable
    PE_CHECK(sel.active());

    PaintToolController tool;
    tool.setBrush(hardBrush(12));
    tool.setColor(kRedF);
    tool.begin(*doc, pt(8, 32), &sel);
    tool.extend(*doc, pt(56, 32));  // sweep across the whole width
    tool.end(*doc);

    PE_CHECK_EQ(alphaAt(*doc, base, 16, 32), 255);  // inside the selection
    PE_CHECK_EQ(alphaAt(*doc, base, 48, 32), 0);    // outside: untouched
}

PE_TEST(painttool_refuses_without_active_pixel_layer) {
    auto doc = Document::createBlank(Size{64, 64});
    doc->setActiveLayer(kNoLayer);  // no paintable target

    PaintToolController tool;
    PE_CHECK(!tool.begin(*doc, pt(32, 32)));
    PE_CHECK(!tool.isStroking());
    // extend/end on a non-started stroke are safe no-ops.
    tool.extend(*doc, pt(40, 40));
    PE_CHECK(!tool.end(*doc));
}

PE_TEST(painttool_begin_while_stroking_is_rejected) {
    auto doc = Document::createBlank(Size{64, 64});
    PaintToolController tool;
    tool.setColor(kRedF);
    PE_CHECK(tool.begin(*doc, pt(10, 10)));
    PE_CHECK(!tool.begin(*doc, pt(20, 20)));  // already stroking
    tool.end(*doc);
}

PE_TEST(painttool_tracks_stroke_dirty_bounds) {
    // The controller accumulates the document-space footprint of the live stroke so
    // a view can recomposite only that region (not the whole canvas) per sample.
    auto doc = Document::createBlank(Size{1024, 1024});

    PaintToolController tool;
    tool.setBrush(hardBrush(16));
    tool.setColor(kRedF);

    PE_CHECK(tool.strokeDirtyBounds().isEmpty());  // no stroke yet

    PE_CHECK(tool.begin(*doc, pt(40, 40)));
    const Rect afterBegin = tool.strokeDirtyBounds();
    PE_CHECK(!afterBegin.isEmpty());
    PE_CHECK(afterBegin.contains(Point{40, 40}));     // covers the first dab
    PE_CHECK(!afterBegin.contains(Point{300, 300}));  // but only the first dab

    tool.extend(*doc, pt(300, 300));
    const Rect afterExtend = tool.strokeDirtyBounds();
    PE_CHECK(afterExtend.contains(Point{40, 40}));  // grew to cover both ends
    PE_CHECK(afterExtend.contains(Point{300, 300}));
    // It is a sub-region of the canvas, not the whole thing — the point of the change.
    PE_CHECK(afterExtend.width < 1024 || afterExtend.height < 1024);

    tool.end(*doc);

    // A fresh stroke resets the accumulator: it must not carry the prior footprint.
    PE_CHECK(tool.begin(*doc, pt(40, 40)));
    PE_CHECK(!tool.strokeDirtyBounds().contains(Point{300, 300}));
    tool.cancel(*doc);
}

PE_TEST(painttool_begin_after_cancel_recovers) {
    // The UI recovers a stuck stroke (lost mouse capture / document swap) by
    // cancelling before starting the next one; a cancelled stroke must leave the
    // controller ready to paint again with nothing committed.
    auto doc = Document::createBlank(Size{64, 64});
    const LayerId base = doc->activeLayer();
    PaintToolController tool;
    tool.setBrush(hardBrush(16));
    tool.setColor(kRedF);

    tool.begin(*doc, pt(8, 8));
    tool.extend(*doc, pt(20, 20));
    tool.cancel(*doc);
    PE_CHECK(!tool.isStroking());
    PE_CHECK_EQ(doc->history().undoDepth(), 0u);

    // A fresh stroke after the cancel works and commits exactly one step.
    PE_CHECK(tool.begin(*doc, pt(40, 40)));
    PE_CHECK(tool.end(*doc));
    PE_CHECK_EQ(alphaAt(*doc, base, 40, 40), 255);
    PE_CHECK_EQ(doc->history().undoDepth(), 1u);
}

// ---------------------------------------------------------------------------
// Per-sample versus cumulative dirty bounds.
//
// LiveStroke::extend() already returns the region a single sample touched, but the
// controller only exposed the running union. Views repainting per sample therefore
// invalidated everything under the whole stroke every time, so repaint cost grew
// with the stroke: quadratic in sample count, and quadratic in area for a diagonal
// drag, since the union is a bounding rectangle.
// ---------------------------------------------------------------------------

PE_TEST(painttool_per_sample_dirty_stays_bounded_while_the_stroke_grows) {
    // A canvas several tiles across, so the cumulative box can grow well past what a
    // single dab can ever touch.
    auto doc = Document::createBlank(Size{2048, 2048});
    PaintToolController tool;
    tool.setBrush(hardBrush(16.0f));
    tool.setColor(kRedF);

    PE_CHECK(tool.begin(*doc, StrokePoint{Vec2{10.0f, 10.0f}, 1.0f}, nullptr));

    int widestSample = tool.lastExtendBounds().width;
    for (int i = 1; i <= 80; ++i) {
        const float d = 10.0f + static_cast<float>(i) * 24.0f;
        tool.extend(*doc, StrokePoint{Vec2{d, d}, 1.0f});
        widestSample = std::max(widestSample, tool.lastExtendBounds().width);
    }

    const Rect cumulative = tool.strokeDirtyBounds();
    // The union spans most of the canvas after a long diagonal drag.
    PE_CHECK(cumulative.width >= 1024);
    // A single sample can never exceed the tiles one dab straddles, which for a brush
    // narrower than a tile is at most two. That ceiling is a constant: it does not
    // move as the stroke gets longer, which is the property that makes repaint linear
    // rather than quadratic. Returning the cumulative box here would blow past it.
    PE_CHECK(widestSample <= 2 * kTileSize);
    PE_CHECK(widestSample < cumulative.width);

    tool.cancel(*doc);
}

PE_TEST(painttool_cumulative_dirty_still_covers_the_whole_stroke) {
    // end() and cancel() need the full footprint, so the cumulative accessor must
    // keep growing even though the per-sample one does not.
    auto doc = Document::createBlank(Size{2048, 2048});
    PaintToolController tool;
    tool.setBrush(hardBrush(16.0f));
    tool.setColor(kRedF);

    PE_CHECK(tool.begin(*doc, StrokePoint{Vec2{20.0f, 20.0f}, 1.0f}, nullptr));
    const Rect afterFirst = tool.strokeDirtyBounds();
    for (int i = 1; i <= 60; ++i) {
        tool.extend(*doc, StrokePoint{Vec2{20.0f + static_cast<float>(i) * 30.0f, 20.0f}, 1.0f});
    }
    const Rect afterMany = tool.strokeDirtyBounds();

    PE_CHECK(afterMany.width > afterFirst.width);
    // And it still contains where the stroke started as well as where it ended.
    PE_CHECK(afterMany.left() <= afterFirst.left());
    PE_CHECK(afterMany.right() >= 1800);

    tool.cancel(*doc);
}

PE_TEST(painttool_stroke_past_the_engine_budget_still_commits_what_it_painted) {
    // The batched modes rebuild the whole stroke every sample, and each of their bake
    // engines refuses outright once the stroke's bounding box passes a budget (heal
    // 2M px, blur/sharpen and mask paint 16M). extend() had already reverted the last
    // good preview before asking for the new one, so crossing that line threw the
    // preview away and end() committed nothing: the entire stroke vanished on release.
    // Freezing at the last representable state loses the tail; losing everything the
    // user just drew is not a recoverable outcome.
    auto doc = Document::createBlank(Size{1600, 1600});
    auto* pl = static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(Rect{0, 0, 1600, 1600}, Rgba8{120, 120, 120, 255});
    pl->tiles().fillRect(Rect{90, 90, 24, 24}, Rgba8{255, 0, 0, 255});  // a blemish to heal

    PaintToolController tool;
    tool.setMode(PaintToolController::Mode::Heal);
    tool.setBrush(hardBrush(24.0f));

    PE_CHECK(tool.begin(*doc, StrokePoint{Vec2{100.0f, 100.0f}, 1.0f}, nullptr));
    PE_CHECK(tool.strokeDirtyBounds().width > 0);  // the first dab previewed
    PE_CHECK(!tool.strokeAtBudget());

    // A long jump that still fits: the inflated heal region is about 1020 px square.
    tool.extend(*doc, StrokePoint{Vec2{1000.0f, 1000.0f}, 1.0f});
    PE_CHECK(!tool.strokeAtBudget());
    const Rect afterFirst = tool.strokeDirtyBounds();
    PE_CHECK(afterFirst.width > 0);

    // And one that does not: about 1520 px square, past the 2M budget.
    tool.extend(*doc, StrokePoint{Vec2{1550.0f, 1550.0f}, 1.0f});
    PE_CHECK(tool.strokeAtBudget());  // frozen rather than blanked

    // The stroke that was already on screen survives the refusal.
    PE_CHECK(tool.strokeDirtyBounds().width >= afterFirst.width);
    PE_CHECK(tool.end(*doc));  // committed, not silently dropped
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));

    // And it is a real edit: the blemish was healed toward its surroundings.
    PE_CHECK(pl->tiles().pixel(100, 100).r < 255);
    doc->history().undo();
    PE_CHECK_EQ(pl->tiles().pixel(100, 100), (Rgba8{255, 0, 0, 255}));  // undo restores it
}
