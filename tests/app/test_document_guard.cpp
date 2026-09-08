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
    //
    // The window has to be SHOWN first. This previously asserted !isVisible() on a window
    // that was never shown, so the condition was already true before the action ran: rewiring
    // Exit straight back to QApplication::quit left the test green, since quit() is a no-op
    // in a suite with no running event loop. That is exactly the regression the test names in
    // its own first line.
    w.show();
    PE_CHECK(w.isVisible());  // and the premise really holds before we trigger
    exitAction->trigger();
    PE_CHECK(!w.isVisible());
}

// The unsaved-changes rule, tested without the modal box that kept it untested. The dialog
// and the save are injected, so each case scripts what the user answered and what the save
// did, and asserts what the caller was told and how many times it was asked.
namespace {

struct DiscardScript {
    std::vector<pe::app::DiscardAnswer> answers;  // consumed in order
    std::vector<bool> saveResults;                // consumed in order
    bool dirty = true;
    // Set when a save "succeeds" but the user painted during it, so the document is still
    // dirty afterwards. That is the case the old code got wrong.
    bool staysDirtyAfterFirstSave = false;
    int asked = 0;
    int saved = 0;

    bool run() {
        return pe::app::resolveUnsavedChanges(
            [this] { return dirty; }, [this] { return nextAnswer(); }, [this] { return doSave(); });
    }
    pe::app::DiscardAnswer nextAnswer() {
        const std::size_t i = static_cast<std::size_t>(asked++);
        return i < answers.size() ? answers[i] : pe::app::DiscardAnswer::Cancel;
    }
    bool doSave() {
        const std::size_t i = static_cast<std::size_t>(saved++);
        const bool ok = i < saveResults.size() ? saveResults[i] : true;
        if (ok && !(staysDirtyAfterFirstSave && i == 0)) dirty = false;
        return ok;
    }
};

}  // namespace

PE_TEST(unsaved_changes_asks_again_when_a_save_leaves_the_document_still_dirty) {
    // The defect this rule exists for. A save serializes a snapshot and leaves the canvas
    // live, so a stroke made while the worker writes is correctly NOT in the file. Returning
    // true because the save succeeded discarded that stroke with no second prompt, on close,
    // New and Open alike.
    DiscardScript s;
    s.answers = {pe::app::DiscardAnswer::Save, pe::app::DiscardAnswer::Save};
    s.saveResults = {true, true};
    s.staysDirtyAfterFirstSave = true;

    PE_CHECK(s.run());
    PE_CHECK_EQ(s.asked, 2);  // asked again about the stroke made during the first save
    PE_CHECK_EQ(s.saved, 2);
    PE_CHECK(!s.dirty);
}

PE_TEST(unsaved_changes_stops_asking_once_nothing_is_unsaved) {
    // The inverse: an ordinary save with no edits during it must not re-prompt, or every
    // close would ask twice.
    DiscardScript s;
    s.answers = {pe::app::DiscardAnswer::Save};
    s.saveResults = {true};

    PE_CHECK(s.run());
    PE_CHECK_EQ(s.asked, 1);
    PE_CHECK_EQ(s.saved, 1);
}

PE_TEST(unsaved_changes_does_not_ask_at_all_when_there_is_nothing_to_lose) {
    DiscardScript s;
    s.dirty = false;
    PE_CHECK(s.run());
    PE_CHECK_EQ(s.asked, 0);
    PE_CHECK_EQ(s.saved, 0);
}

PE_TEST(unsaved_changes_refuses_when_the_save_fails_rather_than_looping) {
    // A failed write, or a Save As the user cancelled. The caller must not proceed, and the
    // prompt must not come back round: a read-only disk would otherwise trap the user in it.
    DiscardScript s;
    s.answers = {pe::app::DiscardAnswer::Save, pe::app::DiscardAnswer::Save};
    s.saveResults = {false};

    PE_CHECK(!s.run());
    PE_CHECK_EQ(s.asked, 1);
    PE_CHECK_EQ(s.saved, 1);
    PE_CHECK(s.dirty);  // and the document still holds the work
}

PE_TEST(unsaved_changes_honours_discard_and_cancel) {
    DiscardScript discard;
    discard.answers = {pe::app::DiscardAnswer::Discard};
    PE_CHECK(discard.run());        // the caller may proceed
    PE_CHECK_EQ(discard.saved, 0);  // without writing anything
    PE_CHECK(discard.dirty);

    DiscardScript cancel;
    cancel.answers = {pe::app::DiscardAnswer::Cancel};
    PE_CHECK(!cancel.run());
    PE_CHECK_EQ(cancel.saved, 0);
}
