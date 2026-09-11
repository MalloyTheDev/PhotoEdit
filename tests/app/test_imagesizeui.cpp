// Image > Image Size. The engine (ResampleDocumentCommand, imageResizeBlocker) is tested
// headlessly in tests/core/test_imagesize.cpp; this covers the dialog that decides what to ask it
// for, and the menu route + refusals that MainWindow wraps around it. applyImageSize is the
// testable half of the split, with the modal exec() the only part these tests do not touch.

#include "ImageSizeDialog.hpp"
#include "MainWindow.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Refusal.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <QAction>
#include <QCheckBox>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSpinBox>
#include <QString>

#include <cstddef>
#include <memory>

namespace {

QString plain(QString s) {
    return s.remove(QLatin1Char('&'));
}

QAction* imageAction(pe::app::MainWindow& w, const QString& label) {
    for (QAction* top : w.menuBar()->actions()) {
        if (top->menu() == nullptr || plain(top->text()) != QStringLiteral("Image")) continue;
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

QString summaryOf(pe::app::ImageSizeDialog& dlg) {
    auto* l = dlg.findChild<QLabel*>(QStringLiteral("ImageSizeSummary"));
    return l != nullptr ? l->text() : QString();
}

}  // namespace

PE_TEST(imagesizeui_the_image_menu_has_image_size_on_its_shortcut) {
    pe::app::MainWindow w;
    QAction* a = imageAction(w, QStringLiteral("Image Size..."));
    PE_REQUIRE(a != nullptr);
    PE_CHECK(a->shortcut() == QKeySequence(QStringLiteral("Ctrl+Alt+I")));
}

PE_TEST(imagesize_dialog_opens_on_the_size_the_document_already_is) {
    pe::app::ImageSizeDialog dlg(nullptr, pe::Size{800, 600});
    PE_CHECK(dlg.size() == (pe::Size{800, 600}));
    PE_CHECK(dlg.constrainProportions());  // on by default

    auto* current = dlg.findChild<QLabel*>(QStringLiteral("ImageSizeCurrent"));
    PE_REQUIRE(current != nullptr);
    PE_CHECK(current->text().contains(QStringLiteral("800")));
    PE_CHECK(current->text().contains(QStringLiteral("600")));

    auto* width = dlg.findChild<QSpinBox*>(QStringLiteral("ImageWidth"));
    PE_REQUIRE(width != nullptr);
    PE_CHECK_EQ(width->value(), 800);
    PE_CHECK_EQ(width->maximum(), pe::kMaxCanvasDimension);
    PE_CHECK_EQ(width->minimum(), 1);
}

PE_TEST(imagesize_dialog_constrains_proportions_until_unlinked) {
    pe::app::ImageSizeDialog dlg(nullptr, pe::Size{800, 600});
    auto* width = dlg.findChild<QSpinBox*>(QStringLiteral("ImageWidth"));
    auto* height = dlg.findChild<QSpinBox*>(QStringLiteral("ImageHeight"));
    PE_REQUIRE(width != nullptr);
    PE_REQUIRE(height != nullptr);

    // Linked: doubling the width takes the height with it (800:600 == 1600:1200).
    width->setValue(1600);
    PE_CHECK_EQ(height->value(), 1200);
    // And it is reversible: halving the width back to 800 restores 600 exactly.
    width->setValue(800);
    PE_CHECK_EQ(height->value(), 600);
    // Editing the height drives the width the same way.
    height->setValue(300);
    PE_CHECK_EQ(width->value(), 400);

    // Unlinked: the two move independently again.
    dlg.setConstrainProportions(false);
    width->setValue(123);
    PE_CHECK_EQ(height->value(), 300);  // unchanged
    PE_CHECK(dlg.size() == (pe::Size{123, 300}));
}

PE_TEST(imagesize_dialog_summary_says_it_resamples_and_warns_when_huge) {
    pe::app::ImageSizeDialog dlg(nullptr, pe::Size{800, 600});
    PE_CHECK(summaryOf(dlg).contains(QStringLiteral("nothing would change")));

    dlg.setConstrainProportions(false);
    dlg.setSize(pe::Size{400, 300});
    QString text = summaryOf(dlg);
    PE_CHECK(text.contains(QStringLiteral("Resampling")));  // not "nothing is resampled"
    PE_CHECK(text.contains(QStringLiteral("400")));
    PE_CHECK(text.contains(QStringLiteral("scaled")));       // says pixels are scaled
    PE_CHECK(!text.contains(QStringLiteral("very large")));  // small: no warning

    dlg.setSize(pe::Size{9000, 9000});  // 81 MP, over kMaxCompositeImagePixels
    PE_CHECK(summaryOf(dlg).contains(QStringLiteral("very large")));
}

PE_TEST(imagesizeui_apply_resamples_the_document_undoable) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{64, 64}, pe::Rgba8{200, 100, 50, 255}), QString());
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_CHECK(w.applyImageSize(pe::Size{128, 128}));
    PE_CHECK(w.document()->canvasSize() == (pe::Size{128, 128}));
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore + 1);

    w.document()->history().undo();
    PE_CHECK(w.document()->canvasSize() == (pe::Size{64, 64}));
}

PE_TEST(imagesizeui_apply_to_the_same_size_is_refused) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{64, 64}, pe::Rgba8{200, 100, 50, 255}), QString());
    w.clearRefusals();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    PE_CHECK(!w.applyImageSize(pe::Size{64, 64}));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore);
}

PE_TEST(imagesizeui_apply_an_out_of_range_size_is_refused) {
    pe::app::MainWindow w;
    w.setDocument(docWith(pe::Size{64, 64}, pe::Rgba8{200, 100, 50, 255}), QString());
    w.clearRefusals();

    PE_CHECK(!w.applyImageSize(pe::Size{0, 128}));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::OverSizeBudget);
    PE_CHECK(w.document()->canvasSize() == (pe::Size{64, 64}));  // untouched
}

PE_TEST(imagesizeui_apply_without_a_document_is_refused) {
    pe::app::MainWindow w;  // no document set
    w.clearRefusals();
    PE_CHECK(!w.applyImageSize(pe::Size{128, 128}));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoDocument);
}
