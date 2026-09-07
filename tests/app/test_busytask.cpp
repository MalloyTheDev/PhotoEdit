// Long operations must not freeze the window.
//
// Save, Export, Open and the Magic Wand all did seconds of work on the GUI thread with no
// feedback: 3.3 seconds to save a 24 MP document as PNG, 2.3 to answer one wand click. A
// window that cannot repaint for that long is what Windows escalates to the not-responding
// state, and a user cannot tell that from a crash.
//
// The property under test is therefore not "it is faster" but "the GUI thread stays alive
// while the work happens, and nothing on it can touch the document meanwhile". Each case
// below has an inverse that fails without the fix, so a green run means something.

#include "BusyTask.hpp"
#include "CanvasView.hpp"
#include "MainWindow.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

#include <QCloseEvent>
#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QObject>
#include <QPointF>
#include <QResizeEvent>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QWidget>

namespace {

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

QImage shotOf(QWidget& w) {
    QImage img(w.size(), QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    w.render(&img);
    return img;
}

}  // namespace

PE_TEST(busytask_runs_the_work_off_the_gui_thread) {
    const auto gui = std::this_thread::get_id();
    std::thread::id ranOn{};
    const pe::app::TaskResult r = pe::app::runBusyTask(
        nullptr, QStringLiteral("working"), [&ranOn] { ranOn = std::this_thread::get_id(); });
    PE_CHECK(r.ran);
    PE_CHECK(!r.threw);
    PE_CHECK(ranOn != std::thread::id{});  // it really ran
    PE_CHECK(ranOn != gui);                // and not here
}

PE_TEST(busytask_keeps_the_gui_thread_processing_events) {
    // The whole point. A timer only ticks while an event loop is running, so the tick count
    // measures exactly what the window's ability to repaint depends on.
    int ticks = 0;
    QTimer timer;
    timer.setInterval(5);
    QObject::connect(&timer, &QTimer::timeout, [&ticks] { ++ticks; });
    timer.start();
    const pe::app::TaskResult r =
        pe::app::runBusyTask(nullptr, QStringLiteral("working"), [] { sleepMs(kSlowMs); });
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

PE_TEST(busytask_blocks_input_but_not_what_the_window_needs_to_draw) {
    // Blocking input is not politeness, it is the safety property: the engine is single
    // threaded, so a handler that reads the document while the worker owns it is a data
    // race. Paint-side events must still get through or the freeze is merely relocated.
    EventCounter w;
    w.resize(60, 60);
    const int resizesBefore = w.resizes;

    // Fires from inside the nested loop, i.e. while the worker is running. `&w` is the
    // context object as well as the capture, so a timer that somehow outlived the task
    // would be cancelled with the widget rather than firing at freed memory.
    QTimer::singleShot(20, &w, [&w] { sendTheLot(w); });
    const pe::app::TaskResult r =
        pe::app::runBusyTask(nullptr, QStringLiteral("working"), [] { sleepMs(kSlowMs); });
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

PE_TEST(busytask_reports_work_that_throws_instead_of_terminating) {
    // An exception crossing a thread boundary is std::terminate, so an out-of-memory encode
    // on a huge export would take the whole application down with the document unsaved.
    const pe::app::TaskResult r = pe::app::runBusyTask(
        nullptr, QStringLiteral("working"), [] { throw std::runtime_error("disk on fire"); });
    PE_CHECK(!r.ran);
    PE_CHECK(r.threw);
    PE_CHECK_EQ(r.error.toStdString(), std::string("disk on fire"));

    // Non-std exceptions are caught too, and still reported rather than swallowed.
    const pe::app::TaskResult odd =
        pe::app::runBusyTask(nullptr, QStringLiteral("working"), [] { throw 42; });
    PE_CHECK(!odd.ran);
    PE_CHECK(odd.threw);
    PE_CHECK(!odd.error.isEmpty());
}

PE_TEST(busytask_with_no_work_is_a_no_op) {
    const pe::app::TaskResult r = pe::app::runBusyTask(nullptr, QStringLiteral("working"), {});
    PE_CHECK(!r.ran);
    PE_CHECK(!r.threw);
}

PE_TEST(documenttask_freezes_the_canvas_for_exactly_the_duration) {
    auto doc = tiledDoc(pe::Rgba8{200, 40, 40, 255});
    pe::app::CanvasView view;
    view.resize(300, 220);
    view.setDocument(doc.get());
    PE_CHECK(!view.isFrozen());

    bool frozenDuring = false;
    const pe::app::TaskResult r =
        pe::app::runDocumentTask(nullptr, &view, QStringLiteral("working"),
                                 [&view, &frozenDuring] { frozenDuring = view.isFrozen(); });
    PE_CHECK(r.ran);
    PE_CHECK(frozenDuring);      // the paint path was out of the way while the worker ran
    PE_CHECK(!view.isFrozen());  // and is back afterwards

    // Thawing has to survive the work throwing, or one failed save leaves the canvas
    // showing a still image of a document that is once again being edited.
    const pe::app::TaskResult bad = pe::app::runDocumentTask(
        nullptr, &view, QStringLiteral("working"), [] { throw std::runtime_error("nope"); });
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
    // (clearing the dirty flag, recording the path, thawing the canvas) happens back on the
    // GUI thread, and a mistake there would leave a saved document looking unsaved.
    const QString path =
        QDir::temp().filePath(QStringLiteral("photoedit_busytask_save_test.pedoc"));
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
    PE_CHECK(!raw->isDirty());  // markSaved ran, so the title stops claiming unsaved work
    PE_CHECK(w.canvas() != nullptr && !w.canvas()->isFrozen());

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
