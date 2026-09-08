#include "BusyTask.hpp"

#include "CanvasView.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMetaObject>
#include <QObject>
#include <QProgressDialog>
#include <QTimer>
#include <QWidget>

#include <exception>
#include <thread>

namespace pe::app {

namespace {

// Swallows the events that could reach a handler which touches what the worker owns.
//
// An application event filter rather than window modality: modality only blocks input to
// OTHER windows and can be bypassed by a shortcut or a posted event, whereas this sees
// everything QCoreApplication::notify dispatches.
//
// The blocked list is explicit rather than a whitelist of what to allow, because getting
// that inverted would stop the window repainting, which is the freeze this exists to fix.
class InputBlocker final : public QObject {
public:
    // `taskWindow` is the window the task was launched from; only that window's close is
    // refused. See the QEvent::Close case.
    InputBlocker(bool blockUserInput, QWidget* taskWindow)
        : blockUserInput_(blockUserInput), taskWindow_(taskWindow) {}

    bool eventFilter(QObject* obj, QEvent* e) override {
        switch (e->type()) {
            case QEvent::MouseButtonPress:
            case QEvent::MouseButtonRelease:
            case QEvent::MouseButtonDblClick:
            case QEvent::MouseMove:
            case QEvent::Wheel:
            case QEvent::KeyPress:
            case QEvent::KeyRelease:
            case QEvent::Shortcut:
            case QEvent::ShortcutOverride:
            case QEvent::TabletPress:
            case QEvent::TabletRelease:
            case QEvent::TabletMove:
            case QEvent::TouchBegin:
            case QEvent::TouchUpdate:
            case QEvent::TouchEnd:
            case QEvent::ContextMenu:
            case QEvent::DragEnter:
            case QEvent::DragMove:
            case QEvent::DragLeave:
            case QEvent::Drop:
                // Let through for a snapshot task: the whole point of taking a snapshot is
                // that the user carries on working while the worker writes.
                return blockUserInput_;
            case QEvent::Close: {
                // Refused in every mode, but only for the window the task belongs to:
                // closing THAT would destroy the document, and the nested event loop, out
                // from under the worker. Every other window is left alone. This filter is
                // installed on the application, and refusing every close application-wide
                // meant a dialog opened during a snapshot save (the Filter and Layer menus
                // stay enabled, by design) could not be dismissed by its own title-bar
                // button, which looks exactly like the hang this code exists to remove.
                //
                // With no window to compare against, fall back to refusing: that is the
                // protective direction, and it is what every caller with a real parent got
                // before.
                auto* w = qobject_cast<QWidget*>(obj);
                if (taskWindow_ != nullptr && (w == nullptr || w->window() != taskWindow_)) {
                    return QObject::eventFilter(obj, e);
                }
                // ignore() and not merely "return true": a QCloseEvent is accepted by
                // default, so swallowing it in a filter leaves QWidget::close() looking at
                // an accepted event and hiding the window anyway. Marking it ignored is
                // what actually refuses the close.
                e->ignore();
                return true;
            }
            default:
                return QObject::eventFilter(obj, e);
        }
    }

private:
    bool blockUserInput_;
    QWidget* taskWindow_;
};

// Installs the input block and (when input is blocked) the wait cursor, and takes them
// away again whatever happens. Not merely tidiness: an exception on the way to starting
// the worker would otherwise leave the application swallowing every click and key for the
// rest of the session, which is a harder lock than the freeze this code exists to remove.
class BlockScope {
public:
    BlockScope(bool blockUserInput, QWidget* taskWindow)
        : app_(QCoreApplication::instance()),
          blocker_(blockUserInput, taskWindow),
          cursor_(blockUserInput) {
        if (app_ != nullptr) app_->installEventFilter(&blocker_);
        // No wait cursor for a snapshot task: the pointer is still a live brush.
        if (cursor_) QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    }
    ~BlockScope() {
        if (cursor_) QGuiApplication::restoreOverrideCursor();
        if (app_ != nullptr) app_->removeEventFilter(&blocker_);
    }
    BlockScope(const BlockScope&) = delete;
    BlockScope& operator=(const BlockScope&) = delete;

private:
    QCoreApplication* app_;
    InputBlocker blocker_;
    bool cursor_;
};

// Freeze the canvas for the duration, and thaw it whatever happens in between: leaving it
// frozen would present a still image of a document that is once again being edited.
class FreezeScope {
public:
    explicit FreezeScope(CanvasView* canvas) : canvas_(canvas) {
        if (canvas_ != nullptr) canvas_->setFrozen(true);
    }
    ~FreezeScope() {
        if (canvas_ != nullptr) canvas_->setFrozen(false);
    }
    FreezeScope(const FreezeScope&) = delete;
    FreezeScope& operator=(const FreezeScope&) = delete;

private:
    CanvasView* canvas_;
};

}  // namespace

TaskResult runDocumentTask(QWidget* parent, CanvasView* canvas, const QString& title,
                           TaskAccess access, const std::function<void()>& work) {
    TaskResult result;
    if (!work) return result;  // nothing to run; not an error, and not a "ran" either

    const bool snapshotTask = access == TaskAccess::Snapshot;
    const BlockScope block(!snapshotTask, parent != nullptr ? parent->window() : nullptr);
    const FreezeScope freeze(access == TaskAccess::LiveDocument ? canvas : nullptr);

    // Indeterminate on purpose: the codecs report no progress, so a percentage would be
    // invented, and an invented one that sticks at 90% is exactly what makes a user
    // believe the app has hung. A moving busy bar claims only that work is happening.
    QProgressDialog dialog(title, QString(), 0, 0, parent);
    dialog.setWindowTitle(title);
    dialog.setCancelButton(nullptr);  // no cancel: the codecs are not interruptible yet
    // Modal only when the work needs the GUI thread to keep its hands off the document. A
    // snapshot task must not take the keyboard away from a canvas the user is painting on,
    // so its dialog neither blocks input nor steals focus when it appears.
    dialog.setWindowModality(snapshotTask ? Qt::NonModal : Qt::ApplicationModal);
    if (snapshotTask) dialog.setAttribute(Qt::WA_ShowWithoutActivating);
    dialog.setMinimumDuration(0);  // shown by the timer below, not by QProgressDialog itself
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    dialog.reset();  // clears the internal show timer armed by the constructor

    QEventLoop loop;
    // Only surface the dialog if the work is actually slow, so a fast save completes
    // without a window appearing at all.
    QTimer::singleShot(kBusyDialogDelayMs, &dialog, [&dialog] { dialog.show(); });

    std::thread worker([&result, &work, &loop] {
        try {
            work();
            result.ran = true;
        } catch (const std::exception& e) {
            result.threw = true;
            result.error = QString::fromUtf8(e.what());
        } catch (...) {
            result.threw = true;
            result.error = QStringLiteral("unknown error");
        }
        // Hop back to the GUI thread to end the nested loop: QEventLoop::quit() is not safe
        // to call from another thread, but posting a metacall to it is. Nothing pumps this
        // thread between here and loop.exec(), so the event cannot be consumed too early.
        QMetaObject::invokeMethod(&loop, [&loop] { loop.quit(); }, Qt::QueuedConnection);
    });
    loop.exec();
    worker.join();  // the synchronization point that makes `result` safe to read

    dialog.hide();
    return result;
}

}  // namespace pe::app
