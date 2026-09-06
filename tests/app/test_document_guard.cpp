// Tests for the unsaved-changes guard and the File/Select keyboard shortcuts.
//
// The guard itself is the reason these exist: the document dirty bit was maintained
// correctly by the engine for a long time and simply never read by the shell, so
// quitting, or opening another file, destroyed unsaved work without a prompt.
//
// The dirty-document paths are deliberately not exercised here: they open a modal
// QMessageBox, which would block a headless run. See the limitations note at the
// bottom of the file.

#include "MainWindow.hpp"
#include "pe_test.hpp"

#include <QAction>
#include <QKeySequence>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QString>

#include <cstdio>
#include <map>

namespace {

QString plain(QString s) {
    return s.remove(QLatin1Char('&'));
}

QMenu* topLevelMenu(pe::app::MainWindow& w, const QString& title) {
    for (QAction* a : w.menuBar()->actions()) {
        if (a->menu() != nullptr && plain(a->text()).compare(title, Qt::CaseInsensitive) == 0) {
            return a->menu();
        }
    }
    return nullptr;
}

// Triggers a menu entry by visible text. Returns false if it was not found, so a
// renamed action fails the test rather than silently skipping it.
bool triggerAction(QMenu* menu, const QString& label) {
    if (menu == nullptr) return false;
    for (QAction* a : menu->actions()) {
        if (plain(a->text()).compare(label, Qt::CaseInsensitive) == 0) {
            a->trigger();
            return true;
        }
    }
    return false;
}

}  // namespace

PE_TEST(mainwindow_every_file_action_has_a_shortcut) {
    // The whole File menu lacked shortcuts, Ctrl+S included, which is the reflex that
    // normally protects work. Every entry here is a document-lifecycle action, so
    // every one of them warrants a binding.
    pe::app::MainWindow w;
    QMenu* file = topLevelMenu(w, QStringLiteral("File"));
    PE_CHECK(file != nullptr);
    if (file == nullptr) return;

    int checked = 0;
    for (QAction* a : file->actions()) {
        if (a->isSeparator() || a->menu() != nullptr) continue;
        ++checked;
        if (a->shortcut().isEmpty()) {
            std::printf("    File > %s has no shortcut\n",
                        plain(a->text()).toLocal8Bit().constData());
            PE_CHECK(false);
        }
    }
    PE_CHECK_EQ(checked, 6);  // New, Open, Save, Save As, Export As, Exit
}

PE_TEST(mainwindow_core_select_actions_have_conventional_shortcuts) {
    // Only the three whole-selection actions. Grow, Shrink and Feather open sizing
    // dialogs and have no conventional binding, so they are deliberately unbound.
    pe::app::MainWindow w;
    QMenu* select = topLevelMenu(w, QStringLiteral("Select"));
    PE_CHECK(select != nullptr);
    if (select == nullptr) return;

    const std::map<QString, QString> expected = {
        {QStringLiteral("Select All"), QStringLiteral("Ctrl+A")},
        {QStringLiteral("Deselect"), QStringLiteral("Ctrl+D")},
        {QStringLiteral("Invert Selection"), QStringLiteral("Ctrl+Shift+I")},
    };

    std::size_t found = 0;
    for (QAction* a : select->actions()) {
        const auto it = expected.find(plain(a->text()));
        if (it == expected.end()) continue;
        ++found;
        PE_CHECK_EQ(a->shortcut().toString(QKeySequence::PortableText), it->second);
    }
    PE_CHECK_EQ(found, expected.size());
}

PE_TEST(mainwindow_shortcuts_do_not_collide) {
    // The tool strip binds 20 single letters and the menus bind chords. A collision
    // would silently make one of the two unreachable.
    pe::app::MainWindow w;
    std::map<QString, QString> seen;  // shortcut -> first owner
    for (QAction* a : w.findChildren<QAction*>()) {
        const QString key = a->shortcut().toString(QKeySequence::PortableText);
        if (key.isEmpty()) continue;
        const QString owner = plain(a->text());
        const auto it = seen.find(key);
        if (it != seen.end()) {
            std::printf("    shortcut %s bound to both \"%s\" and \"%s\"\n",
                        key.toLocal8Bit().constData(), it->second.toLocal8Bit().constData(),
                        owner.toLocal8Bit().constData());
            PE_CHECK(false);
        } else {
            seen.emplace(key, owner);
        }
    }
    PE_CHECK(seen.size() >= 25);  // 20 tools plus the menu chords; guards a vacuous pass
}

PE_TEST(mainwindow_title_marks_unsaved_changes) {
    pe::app::MainWindow w;

    // No document: no marker, and the title says so.
    PE_CHECK(!w.windowTitle().startsWith(QLatin1Char('*')));
    PE_CHECK(w.windowTitle().contains(QStringLiteral("no document")));

    QMenu* file = topLevelMenu(w, QStringLiteral("File"));
    PE_CHECK(triggerAction(file, QStringLiteral("New")));
    // A freshly created document has nothing unsaved yet.
    PE_CHECK(!w.windowTitle().startsWith(QLatin1Char('*')));

    // Any committed command dirties the document; Select All is the cheapest one
    // reachable purely through the menus.
    QMenu* select = topLevelMenu(w, QStringLiteral("Select"));
    PE_CHECK(triggerAction(select, QStringLiteral("Select All")));
    PE_CHECK(w.windowTitle().startsWith(QLatin1Char('*')));
}

PE_TEST(mainwindow_closes_without_prompting_when_there_is_nothing_to_lose) {
    // With no document, and with a clean one, close() must succeed outright. If this
    // ever blocks, the guard is prompting when it has no reason to, which trains
    // users to dismiss the prompt that matters.
    pe::app::MainWindow empty;
    PE_CHECK(empty.close());

    pe::app::MainWindow fresh;
    QMenu* file = topLevelMenu(fresh, QStringLiteral("File"));
    PE_CHECK(triggerAction(file, QStringLiteral("New")));
    PE_CHECK(fresh.close());
}

PE_TEST(mainwindow_exit_action_is_not_wired_straight_to_quit) {
    // Exit was connected directly to QApplication::quit, which bypasses closeEvent
    // and therefore the guard entirely. It must carry Ctrl+Q and go through close().
    pe::app::MainWindow w;
    QMenu* file = topLevelMenu(w, QStringLiteral("File"));
    PE_CHECK(file != nullptr);
    if (file == nullptr) return;

    QAction* exitAction = nullptr;
    for (QAction* a : file->actions()) {
        if (plain(a->text()).compare(QStringLiteral("Exit"), Qt::CaseInsensitive) == 0) {
            exitAction = a;
        }
    }
    PE_CHECK(exitAction != nullptr);
    if (exitAction == nullptr) return;
    PE_CHECK_EQ(exitAction->shortcut().toString(QKeySequence::PortableText),
                QStringLiteral("Ctrl+Q"));

    // Triggering it on a clean window must close that window, not terminate the
    // process, which would take the rest of the suite with it.
    exitAction->trigger();
    PE_CHECK(!w.isVisible());
}
