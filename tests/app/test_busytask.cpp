// Long operations must not freeze the window, and a save must not stop the user working.
//
// Save, Export, Open and the Magic Wand all did seconds of work on the GUI thread with no
// feedback: 3.3 seconds to save a 24 MP document as PNG, 2.3 to answer one wand click. A
// window that cannot repaint for that long is what Windows escalates to the not-responding
// state, and a user cannot tell that from a crash.
//
// Moving the work to a worker is only half of it. The engine is single threaded for reads
// as well as writes, so what the GUI thread is allowed to keep doing depends on what the
// worker owns: an immutable snapshot leaves the canvas fully live, while work against the
// live document has to take the GUI thread off it entirely. TaskAccess names that choice
// and these tests pin what each value actually does.
//
// Each case has an inverse, so a green run means something.

#include "BusyTask.hpp"
#include "CanvasView.hpp"
#include "MainWindow.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#include <QAction>
#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QList>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QObject>
#include <QPointF>
#include <QResizeEvent>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace {

using pe::app::TaskAccess;
using pe::app::TaskResult;

// How long a "slow" task pretends to take. Long enough that a 5 ms timer has many chances
// to tick, short enough that the suite stays quick.
constexpr int kSlowMs = 150;

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Counts the events that actually reach a widget's handlers, by type.
class EventCounter final : public QWidget {
public:
    int presses = 0;
    int moves = 0;
    int keys = 0;
    int closes = 0;
    int resizes = 0;

protected:
    void mousePressEvent(QMouseEvent* e) override {
        ++presses;
        e->accept();
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        ++moves;
        e->accept();
    }
    void keyPressEvent(QKeyEvent* e) override {
        ++keys;
        e->accept();
    }
    void closeEvent(QCloseEvent* e) override {
        ++closes;
        e->accept();
    }
    void resizeEvent(QResizeEvent* e) override {
        ++resizes;
        QWidget::resizeEvent(e);
    }
};

// Send one of each: the three a user generates, the one that would destroy the document,
// and one the window needs in order to keep drawing itself.
void sendTheLot(EventCounter& w) {
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(1, 1), QPointF(1, 1), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&w, &press);
    // With the button held: a drag is the move that matters, because that is what feeds a
    // live stroke into the document.
    QMouseEvent move(QEvent::MouseMove, QPointF(2, 2), QPointF(2, 2), Qt::NoButton, Qt::LeftButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(&w, &move);
    QKeyEvent key(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier);
    QCoreApplication::sendEvent(&w, &key);
    QCloseEvent close;
    QCoreApplication::sendEvent(&w, &close);
    QResizeEvent resize(QSize(61, 61), QSize(60, 60));
    QCoreApplication::sendEvent(&w, &resize);
}

// A document a few tiles across, so anything tiled here crosses a tile boundary.
std::unique_ptr<pe::Document> tiledDoc(pe::Rgba8 fill) {
    auto doc = pe::Document::createBlank(pe::Size{2 * pe::kTileSize, 2 * pe::kTileSize});
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, 2 * pe::kTileSize, 2 * pe::kTileSize}, fill);
    return doc;
}

// A document whose save takes long enough, and reads the document late enough, for an edit
// made at the start of the save to be observable in the file if the save were reading the
// live document.
//
// Both halves matter. Size alone is not enough: most of a .pedoc save is deflate, and the
// pixels are gathered in a burst at the front, so an edit a millisecond in can miss the
// read window entirely and a test built on size alone would pass on a save that read the
// live document. Layers are serialized one at a time, so an edit to the LAST layer is read
// near the end of the save instead. The tests that use this paint into `layers - 1` and
// assert they were inside the task, so a machine that breaks either assumption fails
// loudly rather than passing for the wrong reason.
constexpr int kSlowDocLayers = 6;
constexpr int kSlowDocSide = 1024;

std::unique_ptr<pe::Document> slowToSaveDoc() {
    auto doc = pe::Document::createBlank(pe::Size{kSlowDocSide, kSlowDocSide});
    for (int i = 0; i < kSlowDocLayers; ++i) {
        auto* pl =
            i == 0 ? static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer())) : nullptr;
        std::unique_ptr<pe::PixelLayer> made;
        if (pl == nullptr) {
            made = std::make_unique<pe::PixelLayer>(QStringLiteral("L%1").arg(i).toStdString(),
                                                    pe::BitDepth::U8);
            pl = made.get();
        }
        // Incompressible: deflate is most of the save, so noise is what makes it slow.
        for (int y = 0; y < kSlowDocSide; y += 2) {
            for (int x = 0; x < kSlowDocSide; x += 2) {
                pl->tiles().fillRect(
                    pe::Rect{x, y, 2, 2},
                    pe::Rgba8{static_cast<std::uint8_t>((x * 7 + y * 13 + i * 31) & 0xFF),
                              static_cast<std::uint8_t>((x * 3 + y * 5 + i * 17) & 0xFF),
                              static_cast<std::uint8_t>((x ^ y ^ (i * 91)) & 0xFF), 255});
            }
        }
        if (made != nullptr) {
            const pe::LayerId id = made->id();
            doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(made));
            doc->setActiveLayer(id);  // ends on the topmost, which is serialized last
        }
    }
    return doc;
}

QImage shotOf(QWidget& w) {
    QImage img(w.size(), QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    w.render(&img);
    return img;
}

// The File menu's actions, by the same route a user reaches them.
QList<QAction*> fileActionsOf(pe::app::MainWindow& w) {
    for (QAction* a : w.menuBar()->actions()) {
        if (a->menu() != nullptr && a->text().remove(QLatin1Char('&')) == QStringLiteral("File")) {
            return a->menu()->actions();
        }
    }
    return {};
}

QString tempPath(const char* name) {
    return QDir::temp().filePath(QString::fromUtf8(name));
}

}  // namespace

PE_TEST(task_runs_the_work_off_the_gui_thread) {
    const auto gui = std::this_thread::get_id();
    std::thread::id ranOn{};
    const TaskResult r =
        pe::app::runDocumentTask(nullptr, nullptr, QStringLiteral("working"), TaskAccess::Detached,
                                 [&ranOn] { ranOn = std::this_thread::get_id(); });
    PE_CHECK(r.ran);
    PE_CHECK(!r.threw);
    PE_CHECK(ranOn != std::thread::id{});  // it really ran
    PE_CHECK(ranOn != gui);                // and not here
}

PE_TEST(task_keeps_the_gui_thread_processing_events) {
    // A timer only ticks while an event loop is running, so the tick count measures
    // exactly what the window's ability to repaint depends on.
    int ticks = 0;
    QTimer timer;
    timer.setInterval(5);
    QObject::connect(&timer, &QTimer::timeout, [&ticks] { ++ticks; });
    timer.start();
    const TaskResult r = pe::app::runDocumentTask(nullptr, nullptr, QStringLiteral("working"),
                                                  TaskAccess::Detached, [] { sleepMs(kSlowMs); });
    timer.stop();
    PE_CHECK(r.ran);
    PE_CHECK(ticks > 0);

    // Inverse: the same work done here instead starves the loop completely, which is the
    // bug. Without this the test above would pass on an implementation that changed nothing.
    int starved = 0;
    QTimer blocked;
    blocked.setInterval(5);
    QObject::connect(&blocked, &QTimer::timeout, [&starved] { ++starved; });
    blocked.start();
    sleepMs(kSlowMs);
    blocked.stop();
    PE_CHECK_EQ(starved, 0);
}

PE_TEST(a_live_document_task_blocks_input_but_not_what_the_window_needs_to_draw) {
    // Blocking input is not politeness, it is the safety property: the engine is single
    // threaded, so a handler that reads the live document while the worker owns it is a
    // data race. Paint-side events must still get through or the freeze is merely relocated.
    EventCounter w;
    w.resize(60, 60);
    const int resizesBefore = w.resizes;

    // Fires from inside the nested loop, i.e. while the worker is running. `&w` is the
    // context object as well as the capture, so a timer that somehow outlived the task
    // would be cancelled with the widget rather than firing at freed memory.
    QTimer::singleShot(20, &w, [&w] { sendTheLot(w); });
    const TaskResult r =
        pe::app::runDocumentTask(nullptr, nullptr, QStringLiteral("working"),
                                 TaskAccess::LiveDocument, [] { sleepMs(kSlowMs); });
    PE_CHECK(r.ran);
    PE_CHECK_EQ(w.presses, 0);
    PE_CHECK_EQ(w.moves, 0);
    PE_CHECK_EQ(w.keys, 0);
    PE_CHECK_EQ(w.closes, 0);  // the window cannot be closed out from under the worker
    PE_CHECK(w.resizes > resizesBefore);

    // Inverse: with no task running the same five events all arrive, so the assertions
    // above are about the block and not about the events being undeliverable here.
    sendTheLot(w);
    PE_CHECK_EQ(w.presses, 1);
    PE_CHECK_EQ(w.moves, 1);
    PE_CHECK_EQ(w.keys, 1);
    PE_CHECK_EQ(w.closes, 1);
}

PE_TEST(a_snapshot_task_lets_input_through_and_still_refuses_a_close) {
    // The difference the snapshot buys. The worker owns an isolated copy, so there is
    // nothing for input to corrupt and the user keeps painting. The window still cannot be
    // closed: that would destroy the document and the nested event loop under the worker.
    EventCounter w;
    w.resize(60, 60);

    QTimer::singleShot(20, &w, [&w] { sendTheLot(w); });
    const TaskResult r = pe::app::runDocumentTask(nullptr, nullptr, QStringLiteral("working"),
                                                  TaskAccess::Snapshot, [] { sleepMs(kSlowMs); });
    PE_CHECK(r.ran);
    PE_CHECK_EQ(w.presses, 1);  // the brush still works
    PE_CHECK_EQ(w.moves, 1);    // and so does a drag
    PE_CHECK_EQ(w.keys, 1);
    PE_CHECK_EQ(w.resizes, 1);
    PE_CHECK_EQ(w.closes, 0);  // but the window does not go away mid-save
}

PE_TEST(a_task_refuses_its_own_windows_close_and_leaves_other_windows_alone) {
    // The refusal is installed as an APPLICATION event filter, so it used to swallow every
    // window's close rather than the one window whose destruction would take the document
    // and the nested event loop with it. During a snapshot task the Image/Layer/Select and
    // Filter menus stay enabled by design, so a dialog opened then could not be dismissed by
    // its own title-bar X: the same dead-window symptom this code exists to remove.
    EventCounter task;   // the window the task was launched from
    EventCounter other;  // an unrelated top-level window, e.g. a filter dialog
    task.resize(60, 60);
    other.resize(60, 60);

    QTimer::singleShot(20, &task, [&task, &other] {
        QCloseEvent a;
        QCoreApplication::sendEvent(&task, &a);
        QCloseEvent b;
        QCoreApplication::sendEvent(&other, &b);
    });
    const TaskResult r = pe::app::runDocumentTask(&task, nullptr, QStringLiteral("working"),
                                                  TaskAccess::Snapshot, [] { sleepMs(kSlowMs); });
    PE_CHECK(r.ran);
    PE_CHECK_EQ(task.closes, 0);   // the document window still cannot go away mid-save
    PE_CHECK_EQ(other.closes, 1);  // everything else is left alone
}

PE_TEST(task_reports_work_that_throws_instead_of_terminating) {
    // An exception crossing a thread boundary is std::terminate, so an out-of-memory encode
    // on a huge export would take the whole application down with the document unsaved.
    const TaskResult r =
        pe::app::runDocumentTask(nullptr, nullptr, QStringLiteral("working"), TaskAccess::Detached,
                                 [] { throw std::runtime_error("disk on fire"); });
    PE_CHECK(!r.ran);
    PE_CHECK(r.threw);
    PE_CHECK_EQ(r.error.toStdString(), std::string("disk on fire"));

    // Non-std exceptions are caught too, and still reported rather than swallowed.
    const TaskResult odd = pe::app::runDocumentTask(nullptr, nullptr, QStringLiteral("working"),
                                                    TaskAccess::Detached, [] { throw 42; });
    PE_CHECK(!odd.ran);
    PE_CHECK(odd.threw);
    PE_CHECK(!odd.error.isEmpty());
}

PE_TEST(task_with_no_work_is_a_no_op) {
    const TaskResult r = pe::app::runDocumentTask(nullptr, nullptr, QStringLiteral("working"),
                                                  TaskAccess::Detached, {});
    PE_CHECK(!r.ran);
    PE_CHECK(!r.threw);
}

PE_TEST(only_a_live_document_task_freezes_the_canvas) {
    // The freeze is the expensive half of the contract (the user sees a still image), so it
    // has to be spent only where it is actually needed.
    auto doc = tiledDoc(pe::Rgba8{200, 40, 40, 255});
    pe::app::CanvasView view;
    view.resize(300, 220);
    view.setDocument(doc.get());
    PE_CHECK(!view.isFrozen());

    bool frozenDuringLive = false;
    const TaskResult live = pe::app::runDocumentTask(
        nullptr, &view, QStringLiteral("working"), TaskAccess::LiveDocument,
        [&view, &frozenDuringLive] { frozenDuringLive = view.isFrozen(); });
    PE_CHECK(live.ran);
    PE_CHECK(frozenDuringLive);
    PE_CHECK(!view.isFrozen());

    // A snapshot task must NOT freeze: the canvas keeps compositing the live document
    // while the worker serializes its own copy.
    bool frozenDuringSnapshot = true;
    const TaskResult snap = pe::app::runDocumentTask(
        nullptr, &view, QStringLiteral("working"), TaskAccess::Snapshot,
        [&view, &frozenDuringSnapshot] { frozenDuringSnapshot = view.isFrozen(); });
    PE_CHECK(snap.ran);
    PE_CHECK(!frozenDuringSnapshot);
    PE_CHECK(!view.isFrozen());

    // Thawing has to survive the work throwing, or one failed save leaves the canvas
    // showing a still image of a document that is once again being edited.
    const TaskResult bad =
        pe::app::runDocumentTask(nullptr, &view, QStringLiteral("working"),
                                 TaskAccess::LiveDocument, [] { throw std::runtime_error("no"); });
    PE_CHECK(bad.threw);
    PE_CHECK(!view.isFrozen());

    view.setDocument(nullptr);
}

PE_TEST(a_frozen_canvas_paints_the_last_frame_and_not_the_document) {
    // The freeze is not cosmetic: while frozen, paintEvent must not reach the renderer at
    // all. Proven by changing the document underneath and checking the picture does NOT
    // follow, which is only possible if the paint path stopped reading it.
    auto doc = tiledDoc(pe::Rgba8{220, 30, 30, 255});
    pe::app::CanvasView view;
    view.resize(300, 220);
    view.setDocument(doc.get());
    view.actualPixels();  // 100% zoom, document centred, so the middle pixel is canvas

    const QColor before = shotOf(view).pixelColor(view.width() / 2, view.height() / 2);
    PE_CHECK(before.red() > 150 && before.green() < 100);  // red, as painted

    view.setFrozen(true);
    // Change the document the way a committed edit does, so the renderer's own observer
    // marks the tiles dirty and a live paintEvent WOULD recomposite them. Writing the tiles
    // directly would prove nothing: the cache would still hold the old pixels and the
    // picture would stay red whether the freeze worked or not.
    auto fill = pe::bucketFill(*doc, doc->activeLayer(), 4, 4, pe::Rgbaf{0.1f, 0.8f, 0.25f, 1.0f},
                               255, nullptr);
    PE_CHECK(fill != nullptr);
    if (fill == nullptr) return;
    doc->history().push(std::move(fill));

    const QColor frozen = shotOf(view).pixelColor(view.width() / 2, view.height() / 2);
    PE_CHECK_EQ(frozen.rgb(), before.rgb());

    // Inverse: thawed, the same widget shows the new pixels, so the check above is about
    // the freeze and not about the view being unable to repaint at all.
    view.setFrozen(false);
    const QColor after = shotOf(view).pixelColor(view.width() / 2, view.height() / 2);
    PE_CHECK(after.green() > 150 && after.red() < 100);

    view.setDocument(nullptr);
}

PE_TEST(mainwindow_save_still_works_end_to_end_through_the_worker) {
    // The threading is only worth anything if Save still saves. Everything after the write
    // (recording the saved depth, the path, thawing) happens back on the GUI thread, and a
    // mistake there would leave a saved document looking unsaved.
    const QString path = tempPath("photoedit_busytask_save_test.pedoc");
    QFile::remove(path);

    pe::app::MainWindow w;
    auto doc = tiledDoc(pe::Rgba8{90, 140, 210, 255});
    pe::Document* raw = doc.get();
    w.setDocument(std::move(doc), path);
    // A real edit, so there is something to be dirty about and something to write.
    auto fill = pe::bucketFill(*raw, raw->activeLayer(), 8, 8, pe::Rgbaf{0.9f, 0.2f, 0.2f, 1.0f},
                               255, nullptr);
    PE_CHECK(fill != nullptr);
    if (fill == nullptr) return;
    raw->history().push(std::move(fill));
    PE_CHECK(raw->isDirty());

    PE_CHECK(w.saveDocument());
    PE_CHECK(QFileInfo::exists(path));
    PE_CHECK(QFileInfo(path).size() > 0);
    PE_CHECK(!raw->isDirty());  // nothing changed after the snapshot, so it really is saved
    PE_CHECK(w.canvas() != nullptr && !w.canvas()->isFrozen());
    PE_CHECK(!w.documentTaskInFlight());

    // And what was written is really the document, not an empty file that happens to exist.
    pe::LoadError err = pe::LoadError::None;
    const auto reloaded = pe::loadDocument(path.toStdString(), &err);
    PE_CHECK(reloaded != nullptr);
    if (reloaded != nullptr) {
        PE_CHECK_EQ(reloaded->canvasSize().width, raw->canvasSize().width);
        PE_CHECK_EQ(reloaded->canvasSize().height, raw->canvasSize().height);
    }
    QFile::remove(path);
}

PE_TEST(painting_during_a_save_stays_out_of_the_file_and_leaves_the_document_dirty) {
    // The semantic the whole slice exists to establish:
    //
    //   T0 document is A ... T1 snapshot ... T2 user paints B ... T3 save completes
    //   file  == A
    //   live  == A + B, and still dirty
    //
    // Getting the second line wrong is silent data loss: the window would stop offering to
    // save the strokes made while it was writing.
    const QString path = tempPath("photoedit_busytask_paint_during_save.pedoc");
    QFile::remove(path);

    pe::app::MainWindow w;
    auto doc = slowToSaveDoc();
    pe::Document* raw = doc.get();
    // Paint into the TOP layer, which slowToSaveDoc leaves active and which the serializer
    // reaches last. A save that read the live document would demonstrably pick the new
    // pixels up, rather than getting away with it because it had already passed them.
    constexpr int kPaintX = 900;
    constexpr int kPaintY = 900;
    const pe::Rgba8 before = static_cast<pe::PixelLayer*>(raw->findLayer(raw->activeLayer()))
                                 ->tiles()
                                 .pixel(kPaintX, kPaintY);
    w.setDocument(std::move(doc), path);

    bool paintedInsideTheSave = false;
    bool canvasWasLive = false;
    // Fires from inside the save's nested event loop. Input is not blocked for a snapshot
    // task and timers never are, so this is the user painting mid-save.
    QTimer::singleShot(0, &w, [&] {
        paintedInsideTheSave = w.documentTaskInFlight();
        canvasWasLive = w.canvas() != nullptr && !w.canvas()->isFrozen();
        auto stroke = pe::bucketFill(*raw, raw->activeLayer(), kPaintX, kPaintY,
                                     pe::Rgbaf{0.0f, 1.0f, 0.0f, 1.0f}, 0, nullptr);
        if (stroke != nullptr) raw->history().push(std::move(stroke));
    });

    PE_CHECK(w.saveDocument());
    // If the save outran the timer this proves nothing, so say so rather than pass.
    PE_CHECK(paintedInsideTheSave);
    PE_CHECK(canvasWasLive);  // and the canvas was never frozen while it happened

    const pe::Rgba8 after = static_cast<pe::PixelLayer*>(raw->findLayer(raw->activeLayer()))
                                ->tiles()
                                .pixel(kPaintX, kPaintY);
    PE_CHECK(!(after == before));  // the live document really did change mid-save

    // The file holds the state at snapshot time, not the state at completion.
    pe::LoadError err = pe::LoadError::None;
    const auto reloaded = pe::loadDocument(path.toStdString(), &err);
    PE_CHECK(reloaded != nullptr);
    if (reloaded != nullptr) {
        const pe::Rgba8 onDisk =
            static_cast<const pe::PixelLayer*>(reloaded->findLayer(reloaded->activeLayer()))
                ->tiles()
                .pixel(kPaintX, kPaintY);
        PE_CHECK(onDisk == before);
        PE_CHECK(!(onDisk == after));
    }
    // And the stroke made during the save is unsaved work, so the document says so.
    PE_CHECK(raw->isDirty());
    QFile::remove(path);
}

PE_TEST(the_canvas_runs_its_long_tools_through_the_window_not_around_it) {
    // The Magic Wand called runDocumentTask directly, which is the one thing MainWindow's
    // comment on runGuardedTask says no call site may do. It meant the wand never set
    // documentTaskInFlight_, so the window's close guard was inert for its whole duration,
    // and a wand click during a snapshot save (which deliberately leaves the canvas live)
    // started a second worker and a second nested event loop inside the first.
    pe::app::CanvasView canvas;
    canvas.resize(200, 200);
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()))
        ->tiles()
        .fillRect(pe::Rect{0, 0, 64, 64}, pe::Rgba8{20, 40, 60, 255});
    canvas.setDocument(doc.get());
    canvas.setTool(pe::app::CanvasView::Tool::Wand);

    // A runner that records and refuses, standing in for a window with a task already in
    // flight. If the wand went around it, `asked` would stay 0 and the work would run.
    int asked = 0;
    bool ranTheWork = false;
    canvas.setTaskRunner(
        [&asked, &ranTheWork](const QString&, TaskAccess access, const std::function<void()>&) {
            ++asked;
            PE_CHECK(access == TaskAccess::LiveDocument);  // the wand samples the live renderer
            ranTheWork = false;
            return TaskResult{};  // un-run, exactly what a refused re-entrant task returns
        });

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(100, 100), QPointF(100, 100),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &press);

    PE_CHECK_EQ(asked, 1);                 // it asked the window
    PE_CHECK(!ranTheWork);                 // and took no for an answer
    PE_CHECK(!doc->selection().active());  // so nothing was selected behind the running task
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(0));

    // The inverse: with a runner that actually runs the work, the same click does select.
    canvas.setTaskRunner(
        [](const QString& title, TaskAccess access, const std::function<void()>& work) {
            return pe::app::runDocumentTask(nullptr, nullptr, title, access, work);
        });
    QMouseEvent again(QEvent::MouseButtonPress, QPointF(100, 100), QPointF(100, 100),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&canvas, &again);
    PE_CHECK(doc->selection().active());
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));
}

PE_TEST(file_operations_are_disabled_while_a_document_task_runs) {
    // A snapshot save leaves the canvas live, so the blanket input block is gone and this
    // is what stops a second save, or a New/Open that would replace the document the
    // running save still has to finish against.
    const QString path = tempPath("photoedit_busytask_reentrancy.pedoc");
    QFile::remove(path);

    pe::app::MainWindow w;
    auto doc = slowToSaveDoc();
    w.setDocument(std::move(doc), path);

    const QList<QAction*> before = fileActionsOf(w);
    PE_CHECK(!before.isEmpty());
    int enabledBefore = 0;
    for (QAction* a : before) {
        if (!a->isSeparator() && a->isEnabled()) ++enabledBefore;
    }
    PE_CHECK(enabledBefore > 0);

    int enabledDuring = -1;
    bool sawTaskInFlight = false;
    QTimer::singleShot(0, &w, [&] {
        sawTaskInFlight = w.documentTaskInFlight();
        enabledDuring = 0;
        for (QAction* a : fileActionsOf(w)) {
            if (!a->isSeparator() && a->isEnabled()) ++enabledDuring;
        }
    });

    PE_CHECK(w.saveDocument());
    PE_CHECK(sawTaskInFlight);
    PE_CHECK_EQ(enabledDuring, 0);  // every File action, Exit included

    // And they all come back, or the application would be permanently unable to save.
    int enabledAfter = 0;
    for (QAction* a : fileActionsOf(w)) {
        if (!a->isSeparator() && a->isEnabled()) ++enabledAfter;
    }
    PE_CHECK_EQ(enabledAfter, enabledBefore);
    PE_CHECK(!w.documentTaskInFlight());
    QFile::remove(path);
}

PE_TEST(a_snapshot_task_serializes_the_same_bytes_however_much_the_user_paints) {
    // The paint-during-save story with the timing taken out of it. The worker does not race
    // the painter, it WAITS for it: serialize the snapshot, signal, wait until the GUI
    // thread has committed a real edit, serialize the same snapshot again. Identical bytes
    // are only possible if the worker's document is genuinely isolated from the live one,
    // and the check below that the live document's own bytes DID move rules out the
    // degenerate explanation that nothing changed at all.
    auto doc = tiledDoc(pe::Rgba8{60, 120, 180, 255});
    pe::Document* raw = doc.get();
    pe::app::CanvasView view;
    view.resize(200, 160);
    view.setDocument(raw);

    const std::unique_ptr<const pe::Document> shot = raw->snapshot();
    PE_CHECK(shot != nullptr);
    if (shot == nullptr) {
        view.setDocument(nullptr);
        return;
    }

    std::atomic<bool> serializedOnce{false};
    std::atomic<bool> paintCommitted{false};
    std::vector<std::byte> first;
    std::vector<std::byte> second;
    bool canvasWasLive = false;
    bool paintReallyHappened = false;

    // Runs inside the task's event loop. Spinning here is safe: the worker never needs the
    // GUI thread until it posts the quit at the very end.
    QTimer::singleShot(0, &view, [&] {
        while (!serializedOnce.load(std::memory_order_acquire)) {
        }
        canvasWasLive = !view.isFrozen();
        auto stroke = pe::bucketFill(*raw, raw->activeLayer(), 8, 8,
                                     pe::Rgbaf{0.0f, 1.0f, 0.0f, 1.0f}, 0, nullptr);
        if (stroke != nullptr) {
            raw->history().push(std::move(stroke));
            paintReallyHappened = true;
        }
        paintCommitted.store(true, std::memory_order_release);
    });

    const TaskResult r = pe::app::runDocumentTask(
        nullptr, &view, QStringLiteral("saving"), TaskAccess::Snapshot, [&] {
            first = pe::exportDocument(*shot, pe::ImageFormat::Native);
            serializedOnce.store(true, std::memory_order_release);
            while (!paintCommitted.load(std::memory_order_acquire)) {
            }
            second = pe::exportDocument(*shot, pe::ImageFormat::Native);
        });

    PE_CHECK(r.ran);
    PE_CHECK(paintReallyHappened);  // the edit landed, so there was something to survive
    PE_CHECK(canvasWasLive);        // and the canvas was never frozen while it did
    PE_CHECK(!first.empty());
    PE_CHECK(second == first);  // the worker's document did not move under it

    // And the live document really is different now, so the equality above is isolation
    // rather than nothing having happened.
    const std::vector<std::byte> liveNow = pe::exportDocument(*raw, pe::ImageFormat::Native);
    PE_CHECK(!(liveNow == first));

    view.setDocument(nullptr);
}

PE_TEST(the_window_cannot_be_closed_while_a_save_is_running) {
    // The shutdown contract. A worker is always joined before the call that started it
    // returns, so nothing it holds can outlive the widgets it was launched from; the way
    // that stays true is that the close is REFUSED for the duration rather than deferred.
    // Without this, closing mid-save would destroy the document, the window and the nested
    // event loop the save is running inside.
    const QString path = tempPath("photoedit_busytask_close_during_save.pedoc");
    QFile::remove(path);

    pe::app::MainWindow w;
    auto doc = slowToSaveDoc();
    w.setDocument(std::move(doc), path);
    w.show();

    bool closeRefused = false;
    bool stillVisible = false;
    bool sawTaskInFlight = false;
    QTimer::singleShot(0, &w, [&] {
        sawTaskInFlight = w.documentTaskInFlight();
        closeRefused = !w.close();  // close() reports false when the event is not accepted
        stillVisible = w.isVisible();
    });

    PE_CHECK(w.saveDocument());  // the save still finished
    PE_CHECK(sawTaskInFlight);
    PE_CHECK(closeRefused);
    PE_CHECK(stillVisible);
    PE_CHECK(QFileInfo::exists(path));
    PE_CHECK(!w.documentTaskInFlight());
    QFile::remove(path);
}
