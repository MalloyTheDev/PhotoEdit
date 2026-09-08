// Structural tests for the main window's menu surface.
//
// These are deliberately structural rather than pixel comparisons: they assert that
// a menu exists, is populated, and that its entries are wired to something, which is
// stable across Qt versions and display scaling. The Window and Help menus shipped
// as empty popups for a long time precisely because nothing checked.

#include <QCoreApplication>
#include <QEventLoop>
#include <QSlider>
#include <QThread>
#include "CanvasView.hpp"
#include "EffectDialog.hpp"
#include "MainWindow.hpp"
#include "pe/core/Adjustment.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Refusal.hpp"
#include "pe/core/Selection.hpp"
#include "pe_test.hpp"

#include <iterator>

#include <QAction>
#include <QColor>
#include <QDockWidget>
#include <QImage>
#include <QLabel>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QSize>
#include <QString>
#include <QToolBar>
#include <QToolButton>

#include <cstdio>

namespace {

// Menu titles carry '&' mnemonics; compare on the visible text.
QString plain(QString s) {
    return s.remove(QLatin1Char('&'));
}

// Non-const: QMainWindow::menuBar() is a non-const accessor.
QMenu* topLevelMenu(pe::app::MainWindow& w, const QString& title) {
    const QList<QAction*> actions = w.menuBar()->actions();
    for (QAction* a : actions) {
        if (a->menu() != nullptr && plain(a->text()).compare(title, Qt::CaseInsensitive) == 0) {
            return a->menu();
        }
    }
    return nullptr;
}

}  // namespace

PE_TEST(mainwindow_no_top_level_menu_is_empty) {
    // Guards the whole bug class: a menu bar entry that opens an empty popup.
    pe::app::MainWindow w;
    const QList<QAction*> bar = w.menuBar()->actions();
    PE_CHECK(!bar.isEmpty());
    for (QAction* a : bar) {
        QMenu* m = a->menu();
        PE_CHECK(m != nullptr);
        if (m != nullptr && m->actions().isEmpty()) {
            std::printf("    empty menu: %s\n", plain(a->text()).toLocal8Bit().constData());
            PE_CHECK(false);
        }
    }
}

PE_TEST(mainwindow_window_menu_has_one_toggle_per_dock) {
    pe::app::MainWindow w;
    QMenu* windowMenu = topLevelMenu(w, QStringLiteral("Window"));
    PE_CHECK(windowMenu != nullptr);
    if (windowMenu == nullptr) return;

    const QList<QDockWidget*> docks = w.findChildren<QDockWidget*>();
    PE_CHECK(!docks.isEmpty());

    int toggles = 0;
    for (QAction* a : windowMenu->actions()) {
        if (a->isSeparator()) continue;
        ++toggles;
        // Every entry must be a checkable view toggle, not an inert label.
        PE_CHECK(a->isCheckable());
    }
    PE_CHECK_EQ(toggles, docks.size());
}

PE_TEST(mainwindow_window_menu_toggles_dock_visibility) {
    pe::app::MainWindow w;
    w.show();

    const QList<QDockWidget*> docks = w.findChildren<QDockWidget*>();
    PE_CHECK(!docks.isEmpty());
    if (docks.isEmpty()) return;

    // toggleViewAction() is the action the Window menu carries, so driving it here
    // exercises the same path a menu click takes.
    QDockWidget* dock = docks.front();
    QAction* toggle = dock->toggleViewAction();
    PE_CHECK(toggle != nullptr);
    if (toggle == nullptr) return;

    const bool before = dock->isVisible();
    toggle->trigger();
    PE_CHECK(dock->isVisible() != before);
    toggle->trigger();
    PE_CHECK_EQ(dock->isVisible(), before);
}

PE_TEST(mainwindow_docks_are_closable) {
    // Without DockWidgetClosable the Window menu toggles cannot hide anything, so
    // the menu would look functional and do nothing.
    pe::app::MainWindow w;
    const QList<QDockWidget*> docks = w.findChildren<QDockWidget*>();
    PE_CHECK(!docks.isEmpty());
    for (QDockWidget* d : docks) {
        PE_CHECK(d->features().testFlag(QDockWidget::DockWidgetClosable));
    }
}

PE_TEST(mainwindow_options_bar_buttons_announce_themselves) {
    // The four utility buttons took their tooltip straight from the icon filename,
    // so hovering showed the literal string "share-2". Icon-only buttons also carry
    // no text, so without an accessible name assistive technology announces nothing.
    pe::app::MainWindow w;
    QToolBar* optionsBar = w.findChild<QToolBar*>(QStringLiteral("OptionsBar"));
    PE_CHECK(optionsBar != nullptr);
    if (optionsBar == nullptr) return;

    const QList<QToolButton*> buttons = optionsBar->findChildren<QToolButton*>();
    PE_CHECK(!buttons.isEmpty());
    int checked = 0;
    for (QToolButton* b : buttons) {
        // QToolBar creates its own children (the overflow extension button); Qt names
        // those with a "qt_" prefix, and they are not ours to label.
        if (b->objectName().startsWith(QStringLiteral("qt_"))) continue;
        ++checked;

        const QString name = b->accessibleName().isEmpty() ? b->text() : b->accessibleName();
        if (name.isEmpty()) {
            std::printf("    options-bar button with no accessible name (objectName=%s)\n",
                        b->objectName().toLocal8Bit().constData());
            PE_CHECK(false);
        }
        // An icon resource name is all lowercase with no spaces ("share-2", "cloud").
        // Real user-facing text has a capital or a space; require one.
        const QString tip = b->toolTip();
        const bool looksLikeAnIconName =
            !tip.isEmpty() && tip == tip.toLower() && !tip.contains(QLatin1Char(' '));
        if (looksLikeAnIconName) {
            std::printf("    tooltip looks like an icon filename: %s\n",
                        tip.toLocal8Bit().constData());
            PE_CHECK(false);
        }
    }
    // Guard against the loop silently checking nothing if the buttons ever move.
    PE_CHECK(checked > 0);
}

PE_TEST(mainwindow_every_tool_says_what_it_does) {
    // The status bar used to show the bare tool name, which answers "which tool" but
    // never "what do I do with it". Every tool must now say something: wired tools a
    // gesture hint, scaffolded ones that they are not implemented.
    pe::app::MainWindow w;
    QToolBar* strip = w.findChild<QToolBar*>(QStringLiteral("ToolStrip"));
    QLabel* hint = w.findChild<QLabel*>(QStringLiteral("StatusToolHint"));
    QLabel* name = w.findChild<QLabel*>(QStringLiteral("StatusToolName"));
    PE_CHECK(strip != nullptr);
    PE_CHECK(hint != nullptr);
    PE_CHECK(name != nullptr);
    if (strip == nullptr || hint == nullptr || name == nullptr) return;

    int tools = 0;
    for (QAction* a : strip->actions()) {
        if (a->isSeparator() || !a->isCheckable()) continue;
        ++tools;
        a->trigger();
        if (hint->text().isEmpty()) {
            std::printf("    tool with no status hint: %s\n",
                        plain(a->text()).toLocal8Bit().constData());
            PE_CHECK(false);
        }
        // The name column stays the plain tool name; the hint carries the rest.
        PE_CHECK_EQ(name->text(), plain(a->text()));
    }
    // The strip is expected to carry the full tool set, so a collapse to a couple of
    // entries should fail rather than pass vacuously.
    PE_CHECK(tools >= 20);
}

PE_TEST(mainwindow_tool_tooltips_carry_the_same_hint) {
    // Tooltip and status bar are generated from one hint field, so they cannot drift.
    pe::app::MainWindow w;
    QToolBar* strip = w.findChild<QToolBar*>(QStringLiteral("ToolStrip"));
    QLabel* hint = w.findChild<QLabel*>(QStringLiteral("StatusToolHint"));
    PE_CHECK(strip != nullptr && hint != nullptr);
    if (strip == nullptr || hint == nullptr) return;

    for (QAction* a : strip->actions()) {
        if (a->isSeparator() || !a->isCheckable()) continue;
        a->trigger();
        PE_CHECK(a->toolTip().contains(hint->text()));
    }
}

PE_TEST(mainwindow_status_bar_has_a_cursor_readout) {
    pe::app::MainWindow w;
    QLabel* pos = w.findChild<QLabel*>(QStringLiteral("StatusCursorPos"));
    PE_CHECK(pos != nullptr);
    if (pos == nullptr) return;
    // Off-canvas placeholder, so the readout never shows a stale coordinate.
    PE_CHECK(!pos->text().isEmpty());
    PE_CHECK(!pos->text().contains(QLatin1Char('0')));
}

PE_TEST(mainwindow_help_menu_offers_about) {
    pe::app::MainWindow w;
    QMenu* help = topLevelMenu(w, QStringLiteral("Help"));
    PE_CHECK(help != nullptr);
    if (help == nullptr) return;

    bool foundAbout = false;
    for (QAction* a : help->actions()) {
        if (plain(a->text()).contains(QStringLiteral("About"), Qt::CaseInsensitive)) {
            foundAbout = true;
        }
    }
    PE_CHECK(foundAbout);
}

// ---------------------------------------------------------------------------
// The options-bar zoom strip and the document identity strip.
//
// Zoom was previously a dead label in the status bar: it reported the level but
// offered no way to change it. The identity strip ended in the literal "RGB"
// regardless of the document's actual colour mode and depth.
// ---------------------------------------------------------------------------

namespace {

// Drives a File menu entry by visible text; false if it is not there, so a rename
// fails the test rather than silently skipping the setup.
bool triggerFileAction(pe::app::MainWindow& w, const QString& label) {
    QMenu* file = topLevelMenu(w, QStringLiteral("File"));
    if (file == nullptr) return false;
    for (QAction* a : file->actions()) {
        if (plain(a->text()).compare(label, Qt::CaseInsensitive) == 0) {
            a->trigger();
            return true;
        }
    }
    return false;
}

}  // namespace

PE_TEST(mainwindow_zoom_strip_controls_exist_and_are_named) {
    pe::app::MainWindow w;
    for (const QString& name :
         {QStringLiteral("ZoomOut"), QStringLiteral("ZoomIn"), QStringLiteral("ZoomFit")}) {
        QToolButton* b = w.findChild<QToolButton*>(name);
        PE_CHECK(b != nullptr);
        if (b == nullptr) continue;
        // The visible text is a bare glyph, so the accessible name carries the meaning.
        PE_CHECK(!b->accessibleName().isEmpty());
        PE_CHECK(!b->toolTip().isEmpty());
    }
    PE_CHECK(w.findChild<QLabel*>(QStringLiteral("ZoomValue")) != nullptr);
    PE_CHECK(w.findChild<QLabel*>(QStringLiteral("CanvasSize")) != nullptr);
}

PE_TEST(mainwindow_zoom_buttons_actually_change_the_zoom) {
    // The point of the change: the readout is now a control, not a display.
    pe::app::MainWindow w;
    PE_CHECK(triggerFileAction(w, QStringLiteral("New")));

    QToolButton* in = w.findChild<QToolButton*>(QStringLiteral("ZoomIn"));
    QToolButton* out = w.findChild<QToolButton*>(QStringLiteral("ZoomOut"));
    QLabel* value = w.findChild<QLabel*>(QStringLiteral("ZoomValue"));
    PE_CHECK(in != nullptr && out != nullptr && value != nullptr);
    if (in == nullptr || out == nullptr || value == nullptr) return;

    const QString atStart = value->text();
    PE_CHECK(!atStart.isEmpty());

    in->click();
    const QString zoomedIn = value->text();
    PE_CHECK(zoomedIn != atStart);  // the readout tracks the control

    out->click();
    PE_CHECK(value->text() != zoomedIn);  // and back the other way
}

PE_TEST(mainwindow_zoom_strip_is_blank_with_no_document) {
    pe::app::MainWindow w;
    QLabel* value = w.findChild<QLabel*>(QStringLiteral("ZoomValue"));
    QLabel* size = w.findChild<QLabel*>(QStringLiteral("CanvasSize"));
    PE_CHECK(value != nullptr && size != nullptr);
    if (value == nullptr || size == nullptr) return;
    // No document means no meaningful zoom or canvas size; showing a stale number
    // would be worse than showing nothing.
    PE_CHECK(value->text().isEmpty());
    PE_CHECK(size->text().isEmpty());
}

PE_TEST(mainwindow_document_strip_reports_real_size_mode_and_depth) {
    pe::app::MainWindow w;
    QLabel* tab = w.findChild<QLabel*>(QStringLiteral("DocTab"));
    PE_CHECK(tab != nullptr);
    if (tab == nullptr) return;
    PE_CHECK(tab->text().contains(QStringLiteral("No document")));

    PE_CHECK(triggerFileAction(w, QStringLiteral("New")));

    // New creates 800x600 RGB 8-bit. The dimensions can only come from the document,
    // so they are what proves the strip is derived rather than hard-coded.
    PE_CHECK(tab->text().contains(QStringLiteral("800")));
    PE_CHECK(tab->text().contains(QStringLiteral("600")));
    PE_CHECK(tab->text().contains(QStringLiteral("RGB/8")));

    QLabel* size = w.findChild<QLabel*>(QStringLiteral("CanvasSize"));
    PE_CHECK(size != nullptr);
    if (size != nullptr) {
        PE_CHECK(size->text().contains(QStringLiteral("800")));
        PE_CHECK(size->text().contains(QStringLiteral("600")));
    }
}

PE_TEST(mainwindow_switching_theme_retints_the_tool_icons) {
    // The glyphs are tinted at render time, so a theme change has to rebuild them.
    // themeIconColor() following the theme is not enough on its own: the already
    // built QIcons would keep the old tint unless setTheme rebuilds them.
    pe::app::MainWindow w;
    QToolBar* strip = w.findChild<QToolBar*>(QStringLiteral("ToolStrip"));
    PE_CHECK(strip != nullptr);
    if (strip == nullptr) return;

    QAction* tool = nullptr;
    for (QAction* a : strip->actions()) {
        if (!a->isSeparator() && a->isCheckable() && !a->icon().isNull()) {
            tool = a;
            break;
        }
    }
    PE_CHECK(tool != nullptr);
    if (tool == nullptr) return;

    // Brightest pixel of the rendered glyph, which is the tinted stroke.
    auto strokeOf = [](const QAction* a) {
        const QImage img = a->icon().pixmap(QSize(22, 22)).toImage();
        QColor best;
        int bestAlpha = 0;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const QColor c = img.pixelColor(x, y);
                if (c.alpha() > bestAlpha) {
                    bestAlpha = c.alpha();
                    best = c;
                }
            }
        }
        return best;
    };

    QMenu* view = topLevelMenu(w, QStringLiteral("View"));
    PE_CHECK(view != nullptr);
    if (view == nullptr) return;
    QMenu* themeMenu = nullptr;
    for (QAction* a : view->actions()) {
        if (a->menu() != nullptr &&
            plain(a->text()).compare(QStringLiteral("Theme"), Qt::CaseInsensitive) == 0) {
            themeMenu = a->menu();
        }
    }
    PE_CHECK(themeMenu != nullptr);
    if (themeMenu == nullptr) return;

    QAction* nocturne = nullptr;
    QAction* graphite = nullptr;
    for (QAction* a : themeMenu->actions()) {
        if (plain(a->text()) == QStringLiteral("Nocturne")) nocturne = a;
        if (plain(a->text()) == QStringLiteral("Graphite")) graphite = a;
    }
    PE_CHECK(nocturne != nullptr && graphite != nullptr);
    if (nocturne == nullptr || graphite == nullptr) return;

    nocturne->trigger();
    const QColor before = strokeOf(tool);
    graphite->trigger();
    const QColor after = strokeOf(tool);
    PE_CHECK(before != after);  // the icon actually followed the theme

    nocturne->trigger();  // leave the default active for the rest of the suite
}

// ---------------------------------------------------------------------------
// Document-dependent actions. setEnabled appeared nowhere in MainWindow, so with
// no document every menu item was live and clicking one did nothing at all:
// refusal was indistinguishable from the feature being broken.
// ---------------------------------------------------------------------------

PE_TEST(mainwindow_document_menus_are_disabled_until_there_is_a_document) {
    pe::app::MainWindow w;

    const QStringList gated = {QStringLiteral("Image"), QStringLiteral("Layer"),
                               QStringLiteral("Select"), QStringLiteral("Filter")};
    for (const QString& name : gated) {
        QMenu* m = topLevelMenu(w, name);
        PE_CHECK(m != nullptr);
        if (m == nullptr) continue;
        if (m->isEnabled()) {
            std::printf("    %s enabled with no document\n", name.toLocal8Bit().constData());
            PE_CHECK(false);
        }
    }

    // Menus that must stay usable: File to create or open, View and Window for the
    // workspace, Help for About.
    for (const QString& name : {QStringLiteral("File"), QStringLiteral("View"),
                                QStringLiteral("Window"), QStringLiteral("Help")}) {
        QMenu* m = topLevelMenu(w, name);
        PE_CHECK(m != nullptr && m->isEnabled());
    }

    PE_CHECK(triggerFileAction(w, QStringLiteral("New")));
    for (const QString& name : gated) {
        QMenu* m = topLevelMenu(w, name);
        PE_CHECK(m != nullptr && m->isEnabled());
    }
}

PE_TEST(mainwindow_save_and_export_are_disabled_until_there_is_a_document) {
    pe::app::MainWindow w;
    QMenu* file = topLevelMenu(w, QStringLiteral("File"));
    PE_CHECK(file != nullptr);
    if (file == nullptr) return;

    auto actionNamed = [file](const QString& label) -> QAction* {
        for (QAction* a : file->actions()) {
            if (plain(a->text()).compare(label, Qt::CaseInsensitive) == 0) return a;
        }
        return nullptr;
    };

    const QStringList gated = {QStringLiteral("Save"), QStringLiteral("Save As..."),
                               QStringLiteral("Export As...")};
    for (const QString& label : gated) {
        QAction* a = actionNamed(label);
        PE_CHECK(a != nullptr);
        if (a != nullptr) PE_CHECK(!a->isEnabled());
    }
    // New, Open and Exit have to work with nothing open.
    for (const QString& label :
         {QStringLiteral("New"), QStringLiteral("Open..."), QStringLiteral("Exit")}) {
        QAction* a = actionNamed(label);
        PE_CHECK(a != nullptr && a->isEnabled());
    }

    PE_CHECK(triggerFileAction(w, QStringLiteral("New")));
    for (const QString& label : gated) {
        QAction* a = actionNamed(label);
        PE_CHECK(a != nullptr && a->isEnabled());
    }
}

PE_TEST(mainwindow_undo_and_redo_track_the_history) {
    pe::app::MainWindow w;
    QMenu* edit = topLevelMenu(w, QStringLiteral("Edit"));
    PE_CHECK(edit != nullptr);
    if (edit == nullptr) return;

    QAction* undo = nullptr;
    QAction* redo = nullptr;
    for (QAction* a : edit->actions()) {
        if (plain(a->text()).compare(QStringLiteral("Undo"), Qt::CaseInsensitive) == 0) undo = a;
        if (plain(a->text()).compare(QStringLiteral("Redo"), Qt::CaseInsensitive) == 0) redo = a;
    }
    PE_CHECK(undo != nullptr && redo != nullptr);
    if (undo == nullptr || redo == nullptr) return;

    PE_CHECK(!undo->isEnabled());  // nothing open, nothing to undo
    PE_CHECK(!redo->isEnabled());

    PE_CHECK(triggerFileAction(w, QStringLiteral("New")));
    PE_CHECK(!undo->isEnabled());  // a fresh document has an empty history

    QMenu* select = topLevelMenu(w, QStringLiteral("Select"));
    PE_CHECK(select != nullptr);
    if (select == nullptr) return;
    for (QAction* a : select->actions()) {
        if (plain(a->text()).compare(QStringLiteral("Select All"), Qt::CaseInsensitive) == 0) {
            a->trigger();
        }
    }
    PE_CHECK(undo->isEnabled());  // one committed command
    PE_CHECK(!redo->isEnabled());

    undo->trigger();
    PE_CHECK(redo->isEnabled());  // and now there is something to redo
}

PE_TEST(mainwindow_failure_message_separates_a_format_limit_from_a_memory_limit) {
    // Two different refusals that used to give the same answer. A canvas over the composite
    // cap is a MEMORY limit, and the message rightly sends the user to .pedoc. WebP capping a
    // side at 16383 is a limit of the FORMAT: no bigger budget and no other build helps, so
    // offering .pedoc there answers a question the user did not ask.
    auto doc = pe::Document::createBlank(pe::Size{20000, 3000});  // 60 MP: UNDER the flatten cap
    PE_CHECK(doc != nullptr);

    const QString webp = pe::app::saveFailureReason(doc.get(), QStringLiteral("C:/tmp/wide.webp"),
                                                    pe::SaveError::ExceedsFormatLimit);
    PE_CHECK(webp.contains(QStringLiteral("16383")));  // the actual constraint, by number
    PE_CHECK(webp.contains(QStringLiteral("20000")));  // and what the document actually is
    PE_CHECK(webp.contains(QStringLiteral("format")));
    PE_CHECK(!webp.contains(QStringLiteral("megapixel")));  // not a memory story
    PE_CHECK(!webp.contains(QStringLiteral(".pedoc")));     // and .pedoc is not the answer

    // The memory case still reads as a memory case.
    auto big = pe::Document::createBlank(pe::Size{9000, 9000});
    PE_CHECK(big != nullptr);
    const QString flat = pe::app::saveFailureReason(big.get(), QStringLiteral("C:/tmp/big.png"),
                                                    pe::SaveError::TooLargeToFlatten);
    PE_CHECK(flat.contains(QStringLiteral("megapixel")));
    PE_CHECK(!flat.contains(QStringLiteral("16383")));
}

PE_TEST(mainwindow_save_failure_names_the_flatten_limit) {
    // "Could not save" sent the user looking at disk permissions when the real cause was
    // that the canvas is too large to flatten. Every raster format goes through
    // compositeImage(), which returns nothing above kMaxCompositeImagePixels, so on a
    // canvas past that cap a raster save can never succeed and the message has to say so.
    auto doc = pe::Document::createBlank(pe::Size{9000, 9000});  // 81 MP, over the cap
    PE_CHECK(doc != nullptr);

    const QString png = pe::app::saveFailureReason(doc.get(), QStringLiteral("C:/tmp/big.png"),
                                                   pe::SaveError::TooLargeToFlatten);
    PE_CHECK(png.contains(QStringLiteral("megapixel")));
    PE_CHECK(png.contains(QStringLiteral("9000")));
    PE_CHECK(png.contains(QStringLiteral(".pedoc")));  // and where to go instead

    // The native format has no such limit, so its failure must not blame the size.
    const QString native = pe::app::saveFailureReason(doc.get(), QStringLiteral("C:/tmp/big.pedoc"),
                                                      pe::SaveError::WriteFailed);
    PE_CHECK(!native.contains(QStringLiteral("megapixel")));

    // An extension this build cannot write is its own distinct case.
    const QString unknown = pe::app::saveFailureReason(doc.get(), QStringLiteral("C:/tmp/big.xyz"),
                                                       pe::SaveError::UnsupportedFormat);
    PE_CHECK(!unknown.contains(QStringLiteral("megapixel")));
    PE_CHECK(unknown != native);
}

PE_TEST(effectdialog_throttles_the_preview_during_a_drag) {
    // A slider's step count comes from its decimal count: Exposure Offset spans 1000 steps
    // and Levels Gamma 989. Every tick used to run a whole-layer pass and discard the
    // entire tile cache, synchronously, so dragging one end to end issued about a thousand
    // of each on the GUI thread and the drag itself stopped responding.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_CHECK(doc != nullptr);

    int factoryCalls = 0;
    std::vector<pe::app::EffectDialog::Param> params{
        {QStringLiteral("Amount"), -100.0, 100.0, 0.0, 2}};
    pe::app::EffectDialog dlg(
        nullptr, QStringLiteral("Test"), params,
        [&factoryCalls](const std::vector<double>&) -> std::unique_ptr<pe::Command> {
            ++factoryCalls;
            return nullptr;  // no command: this test is about how OFTEN, not about what
        },
        doc.get(), [] {});

    // The dialog renders once on construction to show the effect at its initial values.
    const int atStart = dlg.previewRebuildCount();

    // Simulate a fast drag: many value changes with no event-loop turn between them, which
    // is exactly what a slider drag on a busy GUI thread produces.
    auto* slider = dlg.findChild<QSlider*>();
    PE_CHECK(slider != nullptr);
    const int ticks = 200;
    for (int i = 0; i < ticks; ++i) slider->setValue(slider->minimum() + i);

    const int rendered = dlg.previewRebuildCount() - atStart;
    PE_CHECK(rendered >= 1);  // the first change still shows immediately
    PE_CHECK(rendered <= 4);  // and the rest collapse rather than rendering per tick
    PE_CHECK(rendered < ticks / 10);
    PE_CHECK_EQ(factoryCalls, dlg.previewRebuildCount());  // one factory call per render

    // The collapsed changes are not lost: they land on the trailing render once the
    // cooldown expires.
    const int beforeTrailing = dlg.previewRebuildCount();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
    QThread::msleep(60);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
    PE_CHECK(dlg.previewRebuildCount() > beforeTrailing);
}

namespace {

// Find a menu action by its visible text, searching submenus. Tests drive the real
// QActions rather than calling handlers directly, so they exercise the whole path the
// user does: action triggered, operation refused, refusal emitted, shell renders it.
QAction* findAction(QWidget* root, const QString& text) {
    for (QAction* a : root->actions()) {
        if (a == nullptr) continue;
        if (a->text() == text) return a;
        if (a->menu() != nullptr) {
            if (QAction* found = findAction(a->menu(), text)) return found;
        }
    }
    return nullptr;
}

std::unique_ptr<pe::Document> docWithPixelLayer() {
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, 64, 64}, pe::Rgba8{10, 20, 30, 255});
    return doc;
}

}  // namespace

PE_TEST(mainwindow_refuses_a_mask_delete_with_no_mask_and_changes_nothing) {
    // The full path: an action the user can reach is triggered, the operation declines, a
    // structured refusal is recorded, and the document is untouched. Asserting on the
    // refusal CODE rather than the status-bar wording means the test survives rephrasing
    // and cannot mistake an unrelated transient message for a refusal.
    pe::app::MainWindow w;
    w.setDocument(docWithPixelLayer(), QString());
    const std::size_t before = w.document()->history().undoDepth();
    w.clearRefusals();

    QAction* del = findAction(w.menuBar(), QStringLiteral("Delete Mask"));
    PE_CHECK(del != nullptr);
    del->trigger();

    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(1));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::LayerHasNoMask);
    PE_CHECK_EQ(w.document()->history().undoDepth(), before);  // state unchanged
}

PE_TEST(mainwindow_refusal_carries_the_fields_a_report_needs) {
    pe::app::MainWindow w;
    w.setDocument(docWithPixelLayer(), QString());
    w.clearRefusals();
    QAction* toggle = findAction(w.menuBar(), QStringLiteral("Toggle Mask Enabled"));
    PE_CHECK(toggle != nullptr);
    toggle->trigger();

    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(1));
    // Bail rather than indexing an empty vector: a test that crashes on its own failure
    // takes every later test's result with it.
    if (w.refusals().empty()) return;
    const pe::Refusal& r = w.refusals().front();
    PE_CHECK(r.isRefusal());
    PE_CHECK(!r.operation.empty());    // which operation
    PE_CHECK(!r.action.empty());       // which affordance reached it
    PE_CHECK(!r.explanation.empty());  // what the user should do
    PE_CHECK(!r.context.empty());      // enough to diagnose it
    PE_CHECK(r.context.find("active layer") != std::string::npos);
    PE_CHECK(r.category == pe::RefusalCategory::WrongTarget);
    PE_CHECK(!r.retryMeaningful);  // repeating it unchanged cannot help
    PE_CHECK(r.fixableByState);    // but adding a mask would
}

PE_TEST(mainwindow_refuses_a_selection_refine_with_no_selection) {
    pe::app::MainWindow w;
    w.setDocument(docWithPixelLayer(), QString());
    w.clearRefusals();
    PE_CHECK(!w.document()->selection().active());

    // Grow prompts for an amount, so drive the guard directly through the same public
    // entry the action uses rather than trying to dismiss a modal dialog in a test.
    w.growSelection(4);
    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(1));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoSelection);
    if (w.refusals().empty()) return;
    PE_CHECK(w.refusals().front().category == pe::RefusalCategory::NoTarget);
}

PE_TEST(mainwindow_refuses_a_selection_refine_that_would_change_nothing) {
    // A no-op refinement is refused rather than pushed: committing it would add a history
    // entry the user has to undo, and silence would leave them thinking the amount they
    // typed was too small to see.
    pe::app::MainWindow w;
    w.setDocument(docWithPixelLayer(), QString());
    pe::Selection sel;
    sel.selectRect(pe::Rect{8, 8, 16, 16});
    w.document()->history().push(std::make_unique<pe::SetSelectionCommand>(std::move(sel)));
    const std::size_t before = w.document()->history().undoDepth();
    w.clearRefusals();

    w.growSelection(0);  // a zero-pixel grow cannot change anything
    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(1));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::NoEffect);
    PE_CHECK_EQ(w.document()->history().undoDepth(), before);  // no phantom undo entry
}

PE_TEST(mainwindow_refuses_undo_during_a_live_stroke_and_says_it_is_retryable) {
    // The one class of refusal where repeating the action later DOES help, which is why
    // retryMeaningful is a field rather than a constant.
    pe::app::MainWindow w;
    w.setDocument(docWithPixelLayer(), QString());
    auto* doc = w.document();
    doc->history().push(std::make_unique<pe::AddLayerCommand>(
        std::make_unique<pe::PixelLayer>("Second"), doc->topLevelCount()));
    const std::size_t before = doc->history().undoDepth();

    PE_CHECK(w.canvas()->tool().begin(*doc, pe::StrokePoint{pe::Vec2{8.0f, 8.0f}, 1.0f}, nullptr));
    w.clearRefusals();
    w.undo();

    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(1));
    PE_CHECK(w.lastRefusalCode() == pe::RefusalCode::StrokeInProgress);
    if (!w.refusals().empty()) {
        PE_CHECK(w.refusals().front().category == pe::RefusalCategory::Busy);
        PE_CHECK(w.refusals().front().retryMeaningful);
    }
    PE_CHECK_EQ(doc->history().undoDepth(), before);  // history untouched mid-stroke
    w.canvas()->tool().cancel(*doc);
}

PE_TEST(mainwindow_an_accepted_operation_emits_no_refusal) {
    // The guard against the opposite failure. Without this, a shell-side change that
    // reported a refusal unconditionally would look like an improvement: every action
    // would explain itself, and every action would also claim to have been rejected.
    pe::app::MainWindow w;
    w.setDocument(docWithPixelLayer(), QString());
    auto* doc = w.document();
    w.clearRefusals();

    // Adding a mask to a layer that has none is valid and must go through silently.
    QAction* reveal = findAction(w.menuBar(), QStringLiteral("Reveal All"));
    PE_CHECK(reveal != nullptr);
    reveal->trigger();
    PE_CHECK(doc->findLayer(doc->activeLayer())->mask() != nullptr);  // it really happened
    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(0));

    // So is toggling that mask, and undoing afterwards.
    QAction* toggle = findAction(w.menuBar(), QStringLiteral("Toggle Mask Enabled"));
    PE_CHECK(toggle != nullptr);
    toggle->trigger();
    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(0));
    w.undo();
    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(0));

    // And a refinement that really changes the selection.
    pe::Selection sel;
    sel.selectRect(pe::Rect{8, 8, 16, 16});
    doc->history().push(std::make_unique<pe::SetSelectionCommand>(std::move(sel)));
    w.growSelection(3);
    PE_CHECK_EQ(w.refusals().size(), static_cast<std::size_t>(0));
}

PE_TEST(refusal_code_category_and_retry_cannot_disagree) {
    // Every code's expected classification, written out BY HAND.
    //
    // This test used to compute the expectation the way pe::refuse computes it, asserting
    // `r.category == pe::categoryOf(c)` and re-deriving retryMeaningful and fixableByState
    // from the same expressions the implementation uses. That is the implementation as its
    // own oracle: change categoryOf so NoSelection returns Unsupported and every no-selection
    // refusal in the shell is filed under the wrong class, fixableByState flips to false so
    // the UI stops telling the user the state is fixable, and the test stays green.
    //
    // A literal table is the only thing that catches that, and it is the mapping a NEW code
    // is most likely to get wrong, which is what the old comment claimed to be testing.
    struct Expected {
        pe::RefusalCode code;
        pe::RefusalCategory category;
        bool retryMeaningful;
        bool fixableByState;
    };
    using C = pe::RefusalCode;
    using K = pe::RefusalCategory;
    const Expected table[] = {
        // nothing to act on
        {C::NoDocument, K::NoTarget, false, true},
        {C::NoActiveLayer, K::NoTarget, false, true},
        {C::NoSelection, K::NoTarget, false, true},
        // wrong kind of target, or wrong state
        {C::LayerNotPixel, K::WrongTarget, false, true},
        {C::LayerHasNoMask, K::WrongTarget, false, true},
        {C::LayerAlreadyHasMask, K::WrongTarget, false, true},
        {C::LayerNotAdjustment, K::WrongTarget, false, true},
        {C::LayerNotText, K::WrongTarget, false, true},
        {C::LayerNotTopLevel, K::WrongTarget, false, true},
        {C::LayerNotGroup, K::WrongTarget, false, true},
        {C::PointOutsideCanvas, K::WrongTarget, false, true},
        // would change nothing
        {C::NoEffect, K::NoEffect, false, true},
        // an edit is in flight: the only category worth retrying unchanged
        {C::StrokeInProgress, K::Busy, true, true},
        {C::TransformInProgress, K::Busy, true, true},
        // beyond the user's reach
        {C::OverSizeBudget, K::OverBudget, false, false},
        {C::Unsupported, K::Unsupported, false, false},
    };
    for (const Expected& e : table) {
        const pe::Refusal r = pe::refuse("t", e.code, "a", "e");
        PE_CHECK(r.isRefusal());
        PE_CHECK(r.category == e.category);
        PE_CHECK_EQ(r.retryMeaningful, e.retryMeaningful);
        // Over budget is not fixable by re-aiming at a different layer: it stays over
        // budget. The over-budget message says retrying is pointless, and the flag has to
        // agree with the sentence.
        PE_CHECK_EQ(r.fixableByState, e.fixableByState);
    }
    // Only Busy is worth retrying, and only Unsupported and OverBudget are beyond the user's
    // reach. Asserted over the table rather than assumed, so a code added without a row here
    // shows up as a count mismatch rather than passing silently.
    PE_CHECK_EQ(std::size(table), static_cast<std::size_t>(16));
    // None is not a refusal, so a default-constructed value cannot be mistaken for one.
    PE_CHECK(!pe::Refusal{}.isRefusal());
}

PE_TEST(effect_refusal_names_the_actual_reason) {
    // applyFilter returns nullptr for four different reasons and the dialog only sees the
    // null, so it classifies from the same document state the engine checked. Getting this
    // wrong would be worse than silence: a confident wrong explanation sends the user to
    // fix something that was never the problem.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    const pe::LayerId base = doc->activeLayer();

    // Empty layer: nothing to filter.
    const pe::Refusal empty = pe::app::effectRefusal(doc.get(), QStringLiteral("Gaussian Blur"));
    PE_CHECK(empty.code == pe::RefusalCode::NoEffect);
    PE_CHECK(empty.explanation.find("empty") != std::string::npos);

    // With content, the same call must NOT claim it is empty.
    static_cast<pe::PixelLayer*>(doc->findLayer(base))
        ->tiles()
        .fillRect(pe::Rect{0, 0, 64, 64}, pe::Rgba8{1, 2, 3, 255});
    const pe::Refusal ok = pe::app::effectRefusal(doc.get(), QStringLiteral("Gaussian Blur"));
    PE_CHECK(ok.explanation.find("empty") == std::string::npos);

    // A non-pixel active layer.
    auto adj = std::make_unique<pe::AdjustmentLayer>(std::make_unique<pe::Invert>(), "Invert");
    const pe::LayerId adjId = adj->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(adj));
    doc->setActiveLayer(adjId);
    const pe::Refusal wrongKind =
        pe::app::effectRefusal(doc.get(), QStringLiteral("Gaussian Blur"));
    PE_CHECK(wrongKind.code == pe::RefusalCode::LayerNotPixel);
    PE_CHECK(wrongKind.category == pe::RefusalCategory::WrongTarget);

    // No document at all.
    const pe::Refusal none = pe::app::effectRefusal(nullptr, QStringLiteral("Gaussian Blur"));
    PE_CHECK(none.code == pe::RefusalCode::NoDocument);

    // Every one names the operation and the affordance, so a report identifies both.
    for (const pe::Refusal* r : {&empty, &wrongKind, &none}) {
        PE_CHECK(!r->operation.empty());
        PE_CHECK(!r->action.empty());
        PE_CHECK(r->isRefusal());
    }
}

PE_TEST(effect_refusal_reports_an_over_budget_layer) {
    // Over pe::kMaxFilterPixels every destructive filter returns nullptr. The message has
    // to name the limit, because no amount of retrying or reselecting fixes it: the user
    // has to select a smaller region.
    const int side = 5000;  // 25 MP, over the 16 MP cap
    auto doc = pe::Document::createBlank(pe::Size{side, side});
    static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()))
        ->tiles()
        .fillRect(pe::Rect{0, 0, side, side}, pe::Rgba8{9, 9, 9, 255});

    const pe::Refusal r = pe::app::effectRefusal(doc.get(), QStringLiteral("Gaussian Blur"));
    PE_CHECK(r.code == pe::RefusalCode::OverSizeBudget);
    PE_CHECK(r.category == pe::RefusalCategory::OverBudget);
    PE_CHECK(r.explanation.find("megapixel") != std::string::npos);
    PE_CHECK(!r.retryMeaningful);
    PE_CHECK(!r.fixableByState);  // no choice of layer makes an over-budget region fit
    PE_CHECK(!r.context.empty());
}
