// View > Proof Colors / Gamut Warning. The colour transform (convertForProof) is tested headlessly
// in the engine; this covers the View menu, the CanvasView display state, and that the proof
// pipeline actually runs (an out-of-gamut colour is flagged). The whole feature is compiled to
// nothing without lcms2, matching the #ifdef guards on the menu and the transform call.

#ifdef PHOTOEDIT_HAVE_LCMS2

#include "CanvasView.hpp"
#include "MainWindow.hpp"
#include "pe/core/ColorProfile.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QString>

#include <cstdlib>
#include <memory>

namespace {

QString plain(QString s) {
    return s.remove(QLatin1Char('&'));
}

QAction* viewAction(pe::app::MainWindow& w, const QString& label) {
    for (QAction* top : w.menuBar()->actions()) {
        if (top->menu() == nullptr || plain(top->text()) != QStringLiteral("View")) continue;
        for (QAction* a : top->menu()->actions()) {
            if (plain(a->text()) == label) return a;
        }
    }
    return nullptr;
}

bool viewHasSubmenu(pe::app::MainWindow& w, const QString& label) {
    for (QAction* top : w.menuBar()->actions()) {
        if (top->menu() == nullptr || plain(top->text()) != QStringLiteral("View")) continue;
        for (QAction* a : top->menu()->actions()) {
            if (a->menu() != nullptr && plain(a->text()) == label) return true;
        }
    }
    return false;
}

}  // namespace

PE_TEST(softproofui_view_menu_has_proof_colors_gamut_and_setup) {
    pe::app::MainWindow w;
    QAction* proof = viewAction(w, QStringLiteral("Proof Colors"));
    QAction* gamut = viewAction(w, QStringLiteral("Gamut Warning"));
    PE_REQUIRE(proof != nullptr);
    PE_REQUIRE(gamut != nullptr);
    PE_CHECK(proof->isCheckable());
    PE_CHECK(gamut->isCheckable());
    PE_CHECK(viewHasSubmenu(w, QStringLiteral("Proof Setup")));
}

PE_TEST(softproofui_menu_toggles_reach_the_canvas) {
    pe::app::MainWindow w;
    w.setDocument(pe::Document::createBlank(pe::Size{8, 8}),
                  QString());  // the toggles are doc-gated
    PE_REQUIRE(w.canvas() != nullptr);
    PE_CHECK(!w.canvas()->proofColors());
    viewAction(w, QStringLiteral("Proof Colors"))->trigger();  // checkable: toggles on
    PE_CHECK(w.canvas()->proofColors());
    PE_CHECK(!w.canvas()->gamutWarning());
    viewAction(w, QStringLiteral("Gamut Warning"))->trigger();
    PE_CHECK(w.canvas()->gamutWarning());
}

PE_TEST(softproof_canvasview_state_setters) {
    auto doc = pe::Document::createBlank(pe::Size{8, 8});  // first: outlives the view
    pe::app::CanvasView view;
    view.setDocument(doc.get());
    PE_CHECK(!view.proofColors());
    view.setProofColors(true);
    PE_CHECK(view.proofColors());
    view.setGamutWarning(true);
    PE_CHECK(view.gamutWarning());
    view.setProofColors(false);
    PE_CHECK(!view.proofColors());
    view.setDocument(nullptr);
}

PE_TEST(softproof_flags_an_out_of_gamut_colour) {
    // A fully saturated ProPhoto green is far outside sRGB. Proofing to sRGB with Gamut Warning on
    // must paint it the alarm colour (mid grey 128) rather than green, which exercises the whole
    // proof pipeline (float composite -> convertForProof) from the app side.
    auto doc = pe::Document::createBlank(pe::Size{8, 8});
    doc->cmdSetColorProfile(pe::ColorProfile::builtin(pe::BuiltinSpace::ProPhotoRGB));
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, 8, 8}, pe::Rgba8{0, 255, 0, 255});

    pe::app::CanvasView view;
    view.setDocument(doc.get());
    view.setProofProfile(pe::ColorProfile::builtin(pe::BuiltinSpace::sRGB));
    view.setGamutWarning(true);
    view.setProofColors(true);
    view.ensureProofImage();

    const pe::PixelBuffer& proofed = view.proofImage();
    PE_REQUIRE(!proofed.isEmpty());
    const pe::Rgba8 p = proofed.at(0, 0);
    // The alarm is Rgbaf{0.5,...} -> ~128 on every channel; a true green would have g >> r,b.
    PE_CHECK(std::abs(static_cast<int>(p.r) - 128) <= 6);
    PE_CHECK(std::abs(static_cast<int>(p.g) - 128) <= 6);
    PE_CHECK(std::abs(static_cast<int>(p.b) - 128) <= 6);

    view.setDocument(nullptr);
}

#endif  // PHOTOEDIT_HAVE_LCMS2
