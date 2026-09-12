// Edit > Assign Profile / Convert to Profile. The colour engine itself is tested headlessly in
// tests/core/test_colorops.cpp; this covers the dialog and the MainWindow wiring that finally
// reaches it. The whole file is compiled to nothing without lcms2 (the feature does not exist
// then), matching the CMake guard on ColorProfileDialog and the MainWindow #ifdefs.

#ifdef PHOTOEDIT_HAVE_LCMS2

#include "ColorProfileDialog.hpp"
#include "MainWindow.hpp"
#include "pe/core/ColorProfile.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Refusal.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
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

std::unique_ptr<pe::Document> docWith(pe::Size size, pe::Rgba8 fill) {
    auto doc = pe::Document::createBlank(size);
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, size.width, size.height}, fill);
    return doc;
}

}  // namespace

PE_TEST(colorui_edit_menu_finally_reaches_the_colour_engine) {
    pe::app::MainWindow w;
    PE_CHECK(editAction(w, QStringLiteral("Assign Profile...")) != nullptr);
    PE_CHECK(editAction(w, QStringLiteral("Convert to Profile...")) != nullptr);
}

PE_TEST(colorprofile_dialog_lists_the_builtin_spaces_and_shows_the_current) {
    pe::app::ColorProfileDialog dlg(nullptr, QStringLiteral("Assign Profile"),
                                    pe::ColorProfile::builtin(pe::BuiltinSpace::sRGB),
                                    /*withConversionOptions=*/false);
    auto* space = dlg.findChild<QComboBox*>(QStringLiteral("ColorProfileSpace"));
    PE_REQUIRE(space != nullptr);
    PE_CHECK_EQ(space->count(), 5);

    auto* current = dlg.findChild<QLabel*>(QStringLiteral("ColorProfileCurrent"));
    PE_REQUIRE(current != nullptr);
    PE_CHECK(current->text().contains(QStringLiteral("Current:")));
    PE_CHECK(!current->text().contains(QStringLiteral("Untagged")));  // a profile was passed

    PE_CHECK(dlg.profile() != nullptr);
    PE_CHECK(dlg.profile()->valid());

    // Assign mode carries no conversion controls.
    PE_CHECK(dlg.findChild<QComboBox*>(QStringLiteral("ColorProfileIntent")) == nullptr);
    PE_CHECK(dlg.findChild<QCheckBox*>(QStringLiteral("ColorProfileBpc")) == nullptr);
}

PE_TEST(colorprofile_dialog_untagged_says_so_and_convert_mode_has_intent_and_bpc) {
    pe::app::ColorProfileDialog dlg(nullptr, QStringLiteral("Convert to Profile"), nullptr,
                                    /*withConversionOptions=*/true);
    auto* current = dlg.findChild<QLabel*>(QStringLiteral("ColorProfileCurrent"));
    PE_REQUIRE(current != nullptr);
    PE_CHECK(current->text().contains(QStringLiteral("Untagged")));  // null current

    auto* intent = dlg.findChild<QComboBox*>(QStringLiteral("ColorProfileIntent"));
    PE_REQUIRE(intent != nullptr);
    PE_CHECK_EQ(intent->count(), 4);
    auto* bpc = dlg.findChild<QCheckBox*>(QStringLiteral("ColorProfileBpc"));
    PE_REQUIRE(bpc != nullptr);
    PE_CHECK(bpc->isChecked());  // on by default
    PE_CHECK(dlg.blackPointCompensation());
    PE_CHECK(dlg.intent() == pe::RenderingIntent::RelativeColorimetric);  // first entry
}

PE_TEST(colorui_assign_tags_the_document_and_undo_untags_it) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{32, 32}, pe::Rgba8{120, 120, 120, 255}), QString());
    PE_CHECK(w.document()->colorProfile() == nullptr);  // new docs are untagged
    const std::size_t undoBefore = w.document()->history().undoDepth();

    auto srgb = pe::ColorProfile::builtin(pe::BuiltinSpace::sRGB);
    PE_CHECK(w.applyAssignProfile(srgb));
    PE_CHECK(w.document()->colorProfile() == srgb);  // tagged with exactly what we passed
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore + 1);

    w.document()->history().undo();
    PE_CHECK(w.document()->colorProfile() == nullptr);  // back to untagged
}

PE_TEST(colorui_convert_needs_a_source_profile_first) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{32, 32}, pe::Rgba8{120, 120, 120, 255}), QString());
    w.clearRefusals();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    // Untagged: there is no source profile to convert from, so Convert refuses (Assign first).
    PE_CHECK(!w.applyConvertProfile(pe::ColorProfile::builtin(pe::BuiltinSpace::DisplayP3),
                                    pe::RenderingIntent::RelativeColorimetric, true));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore);
}

PE_TEST(colorui_convert_retags_a_tagged_document_undoable) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{32, 32}, pe::Rgba8{200, 60, 40, 255}), QString());
    PE_CHECK(w.applyAssignProfile(pe::ColorProfile::builtin(pe::BuiltinSpace::sRGB)));
    const std::size_t afterAssign = w.document()->history().undoDepth();

    auto adobe = pe::ColorProfile::builtin(pe::BuiltinSpace::AdobeRGB1998);
    PE_CHECK(w.applyConvertProfile(adobe, pe::RenderingIntent::RelativeColorimetric, true));
    PE_CHECK(w.document()->colorProfile() == adobe);  // re-tagged with the target
    PE_CHECK_EQ(w.document()->history().undoDepth(), afterAssign + 1);

    w.document()->history().undo();  // convert undone: back to the sRGB tag
    PE_CHECK(w.document()->colorProfile() != adobe);
    PE_CHECK(w.document()->colorProfile() != nullptr);
}

PE_TEST(colorui_assign_without_a_document_is_refused) {
    pe::app::MainWindow w;  // no document
    w.clearRefusals();
    PE_CHECK(!w.applyAssignProfile(pe::ColorProfile::builtin(pe::BuiltinSpace::sRGB)));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoDocument);
}

#endif  // PHOTOEDIT_HAVE_LCMS2
