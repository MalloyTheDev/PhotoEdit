#pragma once

#include <QString>

#include <functional>

class QWidget;

namespace pe::app {

class CanvasView;

// How long a task has to run before the busy dialog appears. Below this the window has not
// visibly stalled and a dialog that flashes up and vanishes is worse than no dialog; above
// it the user needs to be told that something is happening.
inline constexpr int kBusyDialogDelayMs = 250;

// What became of a task run through runBusyTask/runDocumentTask.
struct TaskResult {
    bool ran = false;    // the work function was entered and returned normally
    bool threw = false;  // it left by exception, which the worker caught rather than
                         // letting it cross the thread boundary into std::terminate
    QString error;       // what() when `threw`, else empty
};

// Run `work` on a worker thread while the GUI thread keeps processing events, so a long
// save, export, open or selection cannot make the window stop responding. Returns once the
// work has finished; the caller's control flow is unchanged, which is why the existing
// synchronous error handling around each operation still applies.
//
// THREADING CONTRACT. The engine is single threaded, and not only for writes: tile stores
// and the renderer keep mutable caches that a second reader would race. So while `work`
// runs, the GUI thread must not touch the document at all. Two things enforce that:
//
//   1. Every user input event is swallowed for the duration (mouse, keyboard, tablet,
//      touch, wheel, drag/drop, context menu, and window close), so no handler that reads
//      the document can be entered. Paint, resize, timer and deferred-delete events are
//      deliberately let through: the entire point is that the window keeps drawing itself.
//   2. The caller stops the paint path from reading the document. runDocumentTask does
//      this by freezing the canvas; see CanvasView::setFrozen.
//
// Anything `work` touches must therefore be reachable only from `work`. Do not use this to
// run something that signals back into the widgets while it runs.
[[nodiscard]] TaskResult runBusyTask(QWidget* parent, const QString& title,
                                     const std::function<void()>& work);

// runBusyTask with the canvas frozen for the duration, which is what a task reading or
// writing the SHOWN document needs. Pass `canvas` as null only when the work provably does
// not touch the document the canvas is displaying (opening a file builds a separate
// document, so the canvas can keep painting the current one).
[[nodiscard]] TaskResult runDocumentTask(QWidget* parent, CanvasView* canvas, const QString& title,
                                         const std::function<void()>& work);

}  // namespace pe::app
