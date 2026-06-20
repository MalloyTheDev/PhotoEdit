#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/Mask.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <memory>

using namespace pe;

namespace {
// A 32x32 doc whose single base layer is filled opaque red.
std::unique_ptr<Document> redDoc() {
    auto doc = Document::createBlank(Size{32, 32});
    static_cast<PixelLayer*>(doc->findLayer(doc->activeLayer()))
        ->tiles()
        .fillRect(Rect{0, 0, 32, 32}, Rgba8{255, 0, 0, 255});
    return doc;
}
int compositeAlpha(Document& doc, int x, int y) {
    const PixelBuffer img = doc.compositeImage();
    return img.isEmpty() ? -1 : static_cast<int>(img.at(x, y).a);
}
}  // namespace

PE_TEST(mask_add_reveal_all_is_transparent_no_op_visually) {
    auto doc = redDoc();
    const LayerId base = doc->activeLayer();
    doc->history().push(
        std::make_unique<AddLayerMaskCommand>(base, AddLayerMaskCommand::Init::RevealAll));
    PE_CHECK(doc->findLayer(base)->mask() != nullptr);         // a mask is attached
    PE_CHECK(doc->findLayer(base)->mask()->buffer().empty());  // empty == fully revealing
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 255);            // still fully visible
    doc->history().undo();
    PE_CHECK(doc->findLayer(base)->mask() == nullptr);  // mask removed
    doc->history().redo();
    PE_CHECK(doc->findLayer(base)->mask() != nullptr);  // re-attached
}

PE_TEST(mask_add_hide_all_hides_the_layer) {
    auto doc = redDoc();
    const LayerId base = doc->activeLayer();
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 255);
    doc->history().push(
        std::make_unique<AddLayerMaskCommand>(base, AddLayerMaskCommand::Init::HideAll));
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 0);  // fully masked out
    doc->history().undo();
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 255);  // restored
}

PE_TEST(mask_from_selection_reveals_only_the_selection) {
    auto doc = redDoc();
    const LayerId base = doc->activeLayer();
    Selection sel;
    sel.selectRect(Rect{0, 0, 16, 32});  // left half selected
    doc->editableSelection() = sel;
    doc->history().push(
        std::make_unique<AddLayerMaskCommand>(base, AddLayerMaskCommand::Init::FromSelection));
    PE_CHECK_EQ(compositeAlpha(*doc, 4, 16), 255);  // inside the selection -> revealed
    PE_CHECK_EQ(compositeAlpha(*doc, 28, 16), 0);   // outside -> hidden by the mask
    doc->history().undo();
    PE_CHECK_EQ(compositeAlpha(*doc, 28, 16), 255);  // restored
}

PE_TEST(mask_add_is_noop_when_already_masked) {
    auto doc = redDoc();
    const LayerId base = doc->activeLayer();
    doc->history().push(
        std::make_unique<AddLayerMaskCommand>(base, AddLayerMaskCommand::Init::HideAll));
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 0);
    // A second Add on an already-masked layer is a no-op; its undo must not strip the real mask.
    doc->history().push(
        std::make_unique<AddLayerMaskCommand>(base, AddLayerMaskCommand::Init::RevealAll));
    PE_CHECK(doc->findLayer(base)->mask() != nullptr);
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 0);       // unchanged (still the hide-all mask)
    doc->history().undo();                              // undo the no-op
    PE_CHECK(doc->findLayer(base)->mask() != nullptr);  // original mask intact
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 0);
}

PE_TEST(mask_remove_round_trips) {
    auto doc = redDoc();
    const LayerId base = doc->activeLayer();
    doc->history().push(
        std::make_unique<AddLayerMaskCommand>(base, AddLayerMaskCommand::Init::HideAll));
    doc->history().push(std::make_unique<RemoveLayerMaskCommand>(base));
    PE_CHECK(doc->findLayer(base)->mask() == nullptr);  // removed
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 255);     // visible again
    doc->history().undo();                              // undo the remove
    PE_CHECK(doc->findLayer(base)->mask() != nullptr);  // the hide-all mask is back
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 0);
}

PE_TEST(mask_remove_noop_without_mask) {
    auto doc = redDoc();
    const LayerId base = doc->activeLayer();
    doc->history().push(std::make_unique<RemoveLayerMaskCommand>(base));  // no mask: safe no-op
    PE_CHECK(doc->findLayer(base)->mask() == nullptr);
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 255);
}

PE_TEST(mask_toggle_enabled_round_trips) {
    auto doc = redDoc();
    const LayerId base = doc->activeLayer();
    doc->history().push(
        std::make_unique<AddLayerMaskCommand>(base, AddLayerMaskCommand::Init::HideAll));
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 0);
    doc->history().push(
        std::make_unique<SetMaskEnabledCommand>(base, false));  // disable -> ignored
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 255);
    doc->history().undo();  // re-enable
    PE_CHECK_EQ(compositeAlpha(*doc, 16, 16), 0);
}

PE_TEST(mask_add_noop_on_oversize_canvas) {
    // A canvas larger than the mask buffer's fill cap (~16384 px/side) can't materialize a full
    // mask; HideAll / FromSelection (with an active selection) must NOT attach a misleadingly empty
    // (reveal-all) mask — they no-op. RevealAll always fits (it allocates nothing).
    auto doc = Document::createBlank(Size{20000, 20000});
    const LayerId base = doc->activeLayer();
    doc->history().push(
        std::make_unique<AddLayerMaskCommand>(base, AddLayerMaskCommand::Init::HideAll));
    PE_CHECK(doc->findLayer(base)->mask() == nullptr);  // no misleading reveal-all mask attached

    Selection sel;
    sel.selectRect(Rect{0, 0, 100, 100});
    doc->editableSelection() = sel;
    doc->history().push(
        std::make_unique<AddLayerMaskCommand>(base, AddLayerMaskCommand::Init::FromSelection));
    PE_CHECK(doc->findLayer(base)->mask() ==
             nullptr);  // active selection + oversize canvas -> no-op

    doc->history().push(
        std::make_unique<AddLayerMaskCommand>(base, AddLayerMaskCommand::Init::RevealAll));
    PE_CHECK(doc->findLayer(base)->mask() != nullptr);  // reveal-all always fits
}

PE_TEST(mask_redo_preserves_painted_content) {
    auto doc = redDoc();
    const LayerId base = doc->activeLayer();
    doc->history().push(
        std::make_unique<AddLayerMaskCommand>(base, AddLayerMaskCommand::Init::RevealAll));
    doc->findLayer(base)->mask()->buffer().setValue(5, 5, MaskBuffer::kClear);  // paint a hole
    PE_CHECK_EQ(static_cast<int>(doc->findLayer(base)->mask()->buffer().value(5, 5)), 0);
    doc->history().undo();  // detaches the mask (held by the command)
    PE_CHECK(doc->findLayer(base)->mask() == nullptr);
    doc->history().redo();  // re-attaches the SAME mask object
    PE_CHECK(doc->findLayer(base)->mask() != nullptr);
    PE_CHECK_EQ(static_cast<int>(doc->findLayer(base)->mask()->buffer().value(5, 5)),
                0);  // survived
}
