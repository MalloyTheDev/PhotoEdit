// File > New. Before this, New hard-coded 800x600, so Canvas Size was the only way to any other
// shape. The dialog's exec() is the untestable boundary; the tests reach the two halves it sits
// between - the dialog's accessors, constructed directly, and createNewDocument, which builds
// the document the dialog describes.

#include "MainWindow.hpp"
#include "NewDocumentDialog.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelFormat.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <QComboBox>
#include <QSpinBox>
#include <QString>

#include <cstddef>

namespace {

const pe::PixelLayer* baseLayer(const pe::Document& doc) {
    return static_cast<const pe::PixelLayer*>(doc.findLayer(doc.activeLayer()));
}

}  // namespace

PE_TEST(newdoc_dialog_defaults_to_the_size_new_used_to_hard_code) {
    // 800x600 8-bit is exactly what File > New produced unconditionally, so it stays the
    // default: the change is that it is a starting point now, not the only point.
    pe::app::NewDocumentDialog dlg;
    PE_CHECK(dlg.size() == (pe::Size{800, 600}));
    PE_CHECK(dlg.resolutionPpi() == 72);
    PE_CHECK(dlg.depth() == pe::BitDepth::U8);
    PE_CHECK(dlg.background() == pe::app::NewDocumentDialog::Background::White);
}

PE_TEST(newdoc_dialog_reads_its_fields_back) {
    pe::app::NewDocumentDialog dlg;
    auto* w = dlg.findChild<QSpinBox*>(QStringLiteral("NewWidth"));
    auto* h = dlg.findChild<QSpinBox*>(QStringLiteral("NewHeight"));
    auto* res = dlg.findChild<QSpinBox*>(QStringLiteral("NewResolution"));
    auto* depth = dlg.findChild<QComboBox*>(QStringLiteral("NewDepth"));
    auto* bg = dlg.findChild<QComboBox*>(QStringLiteral("NewBackground"));
    PE_REQUIRE(w != nullptr && h != nullptr && res != nullptr && depth != nullptr && bg != nullptr);

    // The dimension fields carry the engine's own per-side limit, from the shared helper.
    PE_CHECK_EQ(w->maximum(), pe::kMaxCanvasDimension);
    PE_CHECK_EQ(w->minimum(), 1);

    w->setValue(1024);
    h->setValue(768);
    res->setValue(300);
    PE_CHECK(dlg.size() == (pe::Size{1024, 768}));
    PE_CHECK(dlg.resolutionPpi() == 300);

    // Depth and background report the enum stored in the item data, not the row index, so the
    // list could be reordered without changing what a choice means.
    depth->setCurrentIndex(depth->findData(static_cast<int>(pe::BitDepth::F32)));
    PE_CHECK(dlg.depth() == pe::BitDepth::F32);
    bg->setCurrentIndex(
        bg->findData(static_cast<int>(pe::app::NewDocumentDialog::Background::Transparent)));
    PE_CHECK(dlg.background() == pe::app::NewDocumentDialog::Background::Transparent);
}

PE_TEST(newdoc_dialog_presets_fill_the_fields_and_editing_returns_to_custom) {
    pe::app::NewDocumentDialog dlg;
    auto* preset = dlg.findChild<QComboBox*>(QStringLiteral("NewPreset"));
    auto* w = dlg.findChild<QSpinBox*>(QStringLiteral("NewWidth"));
    auto* h = dlg.findChild<QSpinBox*>(QStringLiteral("NewHeight"));
    PE_REQUIRE(preset != nullptr && w != nullptr && h != nullptr);
    PE_CHECK_EQ(preset->currentIndex(), 0);  // Custom to start

    // The 1080p preset (index 1) sets the dimensions...
    preset->setCurrentIndex(1);
    PE_CHECK(dlg.size() == (pe::Size{1920, 1080}));
    // ...and stays named: filling the fields must not immediately bounce the combo back to
    // Custom, or picking a preset would look like it did nothing.
    PE_CHECK_EQ(preset->currentIndex(), 1);

    // Typing a dimension no longer matches the named preset, so it drops back to Custom rather
    // than lying about which preset the fields represent.
    w->setValue(1919);
    PE_CHECK_EQ(preset->currentIndex(), 0);
}

PE_TEST(newdoc_create_makes_a_document_of_the_requested_shape) {
    pe::app::MainWindow win;
    PE_CHECK(win.createNewDocument(pe::Size{320, 240}, pe::BitDepth::U8, 150,
                                   /*whiteBackground=*/false));
    PE_REQUIRE(win.document() != nullptr);
    PE_CHECK(win.document()->canvasSize() == (pe::Size{320, 240}));
    PE_CHECK_EQ(win.document()->resolutionPpi(), 150);
    // No document is dirty the instant it is made.
    PE_CHECK(!win.document()->isDirty());
    PE_CHECK_EQ(win.document()->history().undoDepth(), static_cast<std::size_t>(0));
}

PE_TEST(newdoc_transparent_background_leaves_the_base_layer_empty) {
    // The engine's native blank: a base layer with no tiles. Nothing should have been filled.
    pe::app::MainWindow win;
    PE_CHECK(win.createNewDocument(pe::Size{64, 64}, pe::BitDepth::U8, 72,
                                   /*whiteBackground=*/false));
    PE_REQUIRE(win.document() != nullptr);
    PE_CHECK_EQ(baseLayer(*win.document())->tiles().pixel(10, 10).a, static_cast<uint8_t>(0));
    PE_CHECK_EQ(baseLayer(*win.document())->tiles().tileCount(), static_cast<std::size_t>(0));
}

PE_TEST(newdoc_white_background_fills_the_base_layer_white) {
    pe::app::MainWindow win;
    PE_CHECK(win.createNewDocument(pe::Size{64, 64}, pe::BitDepth::U8, 72,
                                   /*whiteBackground=*/true));
    PE_REQUIRE(win.document() != nullptr);
    const pe::Rgba8 px = baseLayer(*win.document())->tiles().pixel(10, 10);
    PE_CHECK_EQ(px.r, static_cast<uint8_t>(255));
    PE_CHECK_EQ(px.g, static_cast<uint8_t>(255));
    PE_CHECK_EQ(px.b, static_cast<uint8_t>(255));
    PE_CHECK_EQ(px.a, static_cast<uint8_t>(255));  // opaque, or it is not a background
}

PE_TEST(newdoc_white_background_fills_at_the_layer_s_own_depth) {
    // Filling a 16-bit layer with an 8-bit white would band the moment anything is painted over
    // it. The stored value has to be full-scale for the depth.
    pe::app::MainWindow win;
    PE_CHECK(win.createNewDocument(pe::Size{64, 64}, pe::BitDepth::U16, 72,
                                   /*whiteBackground=*/true));
    PE_REQUIRE(win.document() != nullptr);
    const auto* pl = baseLayer(*win.document());
    PE_CHECK(pl->depth() == pe::BitDepth::U16);
    const pe::Rgba16 px = pl->tiles16().pixel(10, 10);
    PE_CHECK_EQ(px.r, static_cast<uint16_t>(65535));
    PE_CHECK_EQ(px.a, static_cast<uint16_t>(65535));
}

PE_TEST(newdoc_create_replaces_the_document_that_was_open) {
    pe::app::MainWindow win;
    PE_CHECK(win.createNewDocument(pe::Size{100, 100}, pe::BitDepth::U8, 72, false));
    const pe::Document* first = win.document();
    PE_REQUIRE(first != nullptr);
    PE_CHECK(win.createNewDocument(pe::Size{200, 200}, pe::BitDepth::U8, 72, false));
    PE_CHECK(win.document()->canvasSize() == (pe::Size{200, 200}));
}
