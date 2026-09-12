// Image Mode / bit depth: SetBitDepthCommand converts the whole document between 8/16/32 bits per
// channel. Widening is lossless; narrowing loses precision, so undo restores a snapshot. These pin
// the per-layer conversion, the document tag, undo (including exact restoration after a narrowing),
// and the all-or-nothing budget refusal.

#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/PixelFormat.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#include <cstdint>
#include <memory>

using namespace pe;

namespace {

PixelLayer* base(Document& doc) {
    return static_cast<PixelLayer*>(doc.findLayer(doc.activeLayer()));
}

SetBitDepthCommand* setDepth(Document& doc, BitDepth target) {
    auto cmd = std::make_unique<SetBitDepthCommand>(target);
    auto* raw = cmd.get();
    doc.history().push(std::move(cmd));
    return raw;
}

}  // namespace

PE_TEST(bitdepth_widen_8_to_16_is_lossless_and_undoable) {
    auto doc = Document::createBlank(Size{8, 8});  // U8
    base(*doc)->tiles().setPixel(0, 0, Rgba8{100, 150, 200, 255});

    auto* cmd = setDepth(*doc, BitDepth::U16);
    PE_CHECK(cmd->converted());
    PE_CHECK(doc->bitDepth() == BitDepth::U16);
    PE_CHECK(base(*doc)->depth() == BitDepth::U16);
    // 8-bit 100 maps exactly to 16-bit 100*257 = 25700 (65535/255 == 257).
    PE_CHECK_EQ(static_cast<int>(base(*doc)->tiles16().pixel(0, 0).r), 25700);
    PE_CHECK_EQ(static_cast<int>(base(*doc)->tiles16().pixel(0, 0).g), 150 * 257);

    doc->history().undo();
    PE_CHECK(doc->bitDepth() == BitDepth::U8);
    PE_CHECK(base(*doc)->depth() == BitDepth::U8);
    PE_CHECK_EQ(base(*doc)->tiles().pixel(0, 0), (Rgba8{100, 150, 200, 255}));
}

PE_TEST(bitdepth_narrow_16_to_8_loses_precision_but_undo_restores_it) {
    auto doc = Document::createBlank(Size{8, 8}, ColorMode::RGB, BitDepth::U16);
    base(*doc)->tiles16().setPixel(0, 0, Rgba16{40000, 20000, 10000, 65535});

    setDepth(*doc, BitDepth::U8);
    PE_CHECK(base(*doc)->depth() == BitDepth::U8);
    // 40000/65535 * 255 ~= 155.6 -> 156, so the 16-bit detail is gone in the 8-bit store.
    PE_CHECK_EQ(static_cast<int>(base(*doc)->tiles().pixel(0, 0).r), 156);

    doc->history()
        .undo();  // the pre-conversion 16-bit value comes back exactly (from the snapshot)
    PE_CHECK(base(*doc)->depth() == BitDepth::U16);
    PE_CHECK_EQ(static_cast<int>(base(*doc)->tiles16().pixel(0, 0).r), 40000);
    PE_CHECK_EQ(static_cast<int>(base(*doc)->tiles16().pixel(0, 0).g), 20000);
}

PE_TEST(bitdepth_convert_to_float_and_back_round_trips_redo) {
    auto doc = Document::createBlank(Size{8, 8});
    base(*doc)->tiles().setPixel(2, 2, Rgba8{64, 128, 255, 255});

    setDepth(*doc, BitDepth::F32);
    PE_CHECK(base(*doc)->depth() == BitDepth::F32);
    const Rgbaf p = base(*doc)->tilesF().pixel(2, 2);
    PE_CHECK(p.b > 0.99f && p.b <= 1.0f);  // 255 -> 1.0

    doc->history().undo();
    PE_CHECK(base(*doc)->depth() == BitDepth::U8);
    doc->history().redo();
    PE_CHECK(base(*doc)->depth() == BitDepth::F32);
    PE_CHECK(base(*doc)->tilesF().pixel(2, 2).b > 0.99f);
}

PE_TEST(bitdepth_blocker_reports_unchanged_and_ok) {
    auto doc = Document::createBlank(Size{8, 8});  // U8
    PE_CHECK(bitDepthBlocker(*doc, BitDepth::U8) == BitDepthBlock::Unchanged);
    PE_CHECK(bitDepthBlocker(*doc, BitDepth::U16) == BitDepthBlock::None);
    PE_CHECK(bitDepthBlocker(*doc, BitDepth::F32) == BitDepthBlock::None);
}

PE_TEST(bitdepth_unchanged_is_a_noop) {
    auto doc = Document::createBlank(Size{8, 8});
    const std::size_t undoBefore = doc->history().undoDepth();
    auto* cmd = setDepth(*doc, BitDepth::U8);  // already U8
    PE_CHECK(!cmd->converted());
    PE_CHECK(doc->bitDepth() == BitDepth::U8);
    (void)undoBefore;
}

PE_TEST(bitdepth_converts_layers_inside_groups) {
    auto doc = Document::createBlank(Size{8, 8});
    auto group = std::make_unique<GroupLayer>("Grp");
    auto child = std::make_unique<PixelLayer>("Inner");  // U8 by default
    child->tiles().setPixel(1, 1, Rgba8{10, 20, 30, 255});
    const LayerId childId = child->id();
    group->addChild(std::move(child));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));

    setDepth(*doc, BitDepth::U16);
    PE_CHECK(static_cast<PixelLayer*>(doc->findLayer(childId))->depth() == BitDepth::U16);
    doc->history().undo();
    PE_CHECK(static_cast<PixelLayer*>(doc->findLayer(childId))->depth() == BitDepth::U8);
}

PE_TEST(bitdepth_retained_bytes_counts_the_snapshot) {
    auto doc = Document::createBlank(Size{64, 64});
    base(*doc)->tiles().fillRect(Rect{0, 0, 64, 64}, Rgba8{80, 80, 80, 255});
    auto* cmd = setDepth(*doc, BitDepth::U16);
    PE_CHECK(cmd->converted());
    PE_CHECK(cmd->retainedBytes() > 0);  // holds the pre-conversion 8-bit pixels for undo
}

PE_TEST(bitdepth_refuses_content_too_large_to_convert) {
    auto doc = Document::createBlank(Size{40000, 40000});
    base(*doc)->tiles().setPixel(0, 0, Rgba8{1, 2, 3, 255});
    base(*doc)->tiles().setPixel(39999, 39999, Rgba8{4, 5, 6, 255});  // content bbox ~40000^2
    // At F32 (16 bytes/px) that store is far over the move budget.
    PE_CHECK(bitDepthBlocker(*doc, BitDepth::F32) == BitDepthBlock::ContentTooLarge);
    auto* cmd = setDepth(*doc, BitDepth::F32);
    PE_CHECK(!cmd->converted());
    PE_CHECK(doc->bitDepth() == BitDepth::U8);  // untouched
}
