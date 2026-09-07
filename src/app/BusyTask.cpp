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

// Swallows every event that could reach a handler which reads the document, for as long as
// a worker owns it. This is the guard that matters: window modality only blocks input to
// OTHER windows and can be bypassed by a shortcut or a posted event, whereas an application
// event filter sees everything QCoreApplication::notify dispatches.
//
// The blocked list is explicit rather than a whitelist of what to allow, because getting
// that inverted would stop the window repainting, which is the freeze this exists to fix.
class InputBlocker final : public QObject {
public:
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
                return true;
            case QEvent::Close:
                // Closing the window would destroy the document out from under the worker.
                // Refusing the close is the only safe answer; the user can close once the
                // save finishes, which is a second or two away.
                return true;
            default:
                return QObject::eventFilter(obj, e);
        }
    }
};

// Installs the input block and the wait cursor, and takes them away again whatever happens.
// Not merely tidiness: an exception on the way to starting the worker would otherwise leave
// the application swallowing every click and key for the rest of the session, which is a
// harder lock than the freeze this code exists to remove.
class BlockScope {
public:
    BlockScope() : app_(QCoreApplication::instance()) {
        if (app_ != nullptr) app_->installEventFilter(&blocker_);
        QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    }
    ~BlockScope() {
        QGuiApplication::restoreOverrideCursor();
        if (app_ != nullptr) app_->removeEventFilter(&blocker_);
    }
    BlockScope(const BlockScope&) = delete;
    BlockScope& operator=(const BlockScope&) = delete;

private:
    QCoreApplication* app_;
    InputBlocker blocker_;
};

}  // namespace

TaskResult runBusyTask(QWidget* parent, const QString& title, const std::function<void()>& work) {
    TaskResult result;
    if (!work) return result;  // nothing to run; not an error, and not a "ran" either

    const BlockScope block;

    // Indeterminate on purpose: the codecs report no progress, so a percentage would be
    // invented, and an invented one that sticks at 90% is exactly what makes a user
    // believe the app has hung. A moving busy bar claims only that work is happening.
    QProgressDialog dialog(title, QString(), 0, 0, parent);
    dialog.setWindowTitle(title);
    dialog.setCancelButton(nullptr);  // no cancel: the codecs are not interruptible yet
    dialog.setWindowModality(Qt::ApplicationModal);
    dialog.setMinimumDuration(0);  // shown by the timer below, not by QProgressDialog itself
    dialog.setAutoClose(false);
    dialog.setAutoReset(false);
    dialog.reset();  // clears the internal show timer armed by the constructor

    QEventLoop loop;
    // Only surface the dialog if the work is actually slow. Input is already blocked either
    // way, so a fast save simply completes without a window appearing at all.
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

TaskResult runDocumentTask(QWidget* parent, CanvasView* canvas, const QString& title,
                           const std::function<void()>& work) {
    // Freeze first and thaw last, whatever happens in between: leaving the canvas frozen
    // would present a still image of a document that is once again being edited.
    struct FreezeGuard {
        CanvasView* canvas;
        explicit FreezeGuard(CanvasView* c) : canvas(c) {
            if (canvas != nullptr) canvas->setFrozen(true);
        }
        ~FreezeGuard() {
            if (canvas != nullptr) canvas->setFrozen(false);
        }
        FreezeGuard(const FreezeGuard&) = delete;
        FreezeGuard& operator=(const FreezeGuard&) = delete;
    } guard(canvas);

    return runBusyTask(parent, title, work);
}

}  // namespace pe::app
