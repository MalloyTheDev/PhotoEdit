#include "pe/core/Brush.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PaintToolController.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

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
