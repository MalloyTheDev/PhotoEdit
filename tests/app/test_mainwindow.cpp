// Structural tests for the main window's menu surface.
//
// These are deliberately structural rather than pixel comparisons: they assert that
// a menu exists, is populated, and that its entries are wired to something, which is
// stable across Qt versions and display scaling. The Window and Help menus shipped
// as empty popups for a long time precisely because nothing checked.

#include "MainWindow.hpp"
#include "pe_test.hpp"

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
