#pragma once

#include <QString>

#include <cstdint>
#include <functional>

class QWidget;

namespace pe::app {

class CanvasView;

// How long a task has to run before the busy dialog appears. Below this the window has not
// visibly stalled and a dialog that flashes up and vanishes is worse than no dialog; above
// it the user needs to be told that something is happening.
inline constexpr int kBusyDialogDelayMs = 250;

// What a background task is allowed to reach, which decides what the GUI thread has to stop
// doing while it runs. The caller states this rather than assembling a policy, because the
// two halves (blocking input, freezing the canvas) are only correct together and only for
// the right kind of work.
enum class TaskAccess : std::uint8_t {
    // The work owns an immutable pe::Document snapshot and touches nothing else. Tile
    // buffers are shared copy-on-write and the live document forks before writing them, so
    // there is nothing to protect: the canvas keeps compositing and the user keeps
    // painting. This is what Save and Export use.
    Snapshot,

    // The work reads or writes the LIVE document. The engine is single threaded for reads
    // as well as writes (tile stores cache their bounds lazily, the renderer owns a mutable
    // LRU), so the GUI thread must not touch it at all: input is blocked and the canvas is
    // frozen to its last frame. This is what the Magic Wand uses, because it deliberately
    // samples the renderer's warm tile cache rather than a snapshot's cold one.
    LiveDocument,

    // The work touches no document the GUI can reach (opening a file builds a new one).
    // The canvas keeps painting the current document, but input is blocked, because that
    // document is about to be replaced and edits made in the meantime would be discarded.
    Detached,
};

// What became of a task run through runDocumentTask.
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
// THREADING CONTRACT. What the GUI thread gives up for the duration is `access`, above.
// Whatever is blocked, these always are:
//
//   - Window close. Closing would destroy the document, and in the Snapshot case the
//     nested event loop, out from under the worker. The close is refused, not deferred;
//     the user can close once the task finishes. This is the shutdown contract: a worker
//     is always joined before the call that started it returns, so nothing the worker
//     holds can outlive the GUI objects it was launched from.
//   - Paint, resize and timer events are never blocked, in any mode. Keeping the window
//     drawing itself is the entire point.
//
// `work` must not signal back into the widgets while it runs; the GUI thread is inside a
// nested event loop and a blocking call back into it would deadlock.
[[nodiscard]] TaskResult runDocumentTask(QWidget* parent, CanvasView* canvas, const QString& title,
                                         TaskAccess access, const std::function<void()>& work);

}  // namespace pe::app
