// Structural tests for the main window's menu surface.
//
// These are deliberately structural rather than pixel comparisons: they assert that
// a menu exists, is populated, and that its entries are wired to something, which is
// stable across Qt versions and display scaling. The Window and Help menus shipped
// as empty popups for a long time precisely because nothing checked.

#include "MainWindow.hpp"
#include "pe_test.hpp"

#include <QAction>
#include <QDockWidget>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QString>

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
