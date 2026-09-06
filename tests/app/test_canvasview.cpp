// Canvas feedback for gestures that cannot do anything.
//
// A brush press on a layer the paint controller refuses (an adjustment layer, or no
// layer at all) used to be completely silent: nothing changed and nothing was said,
// which looks exactly like a broken brush. Bucket and Gradient already reported it.

#include "CanvasView.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Document.hpp"
#include "pe_test.hpp"

#include <memory>
#include <utility>

#include <QCoreApplication>
#include <QMouseEvent>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QStringList>

namespace {

// Every toolMessage the view emits while `body` runs.
template <typename F>
QStringList messagesFrom(pe::app::CanvasView& view, F body) {
    QStringList seen;
    const QMetaObject::Connection c = QObject::connect(&view, &pe::app::CanvasView::toolMessage,
                                                       [&seen](const QString& m) { seen << m; });
    body();
    QObject::disconnect(c);
    return seen;
}

// A left-button press at the centre of the (unshown, default-sized) view.
void pressLeft(pe::app::CanvasView& view) {
    const QPointF at(static_cast<qreal>(view.width()) / 2.0,
                     static_cast<qreal>(view.height()) / 2.0);
    QMouseEvent ev(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton,
                   Qt::NoModifier);
    QCoreApplication::sendEvent(&view, &ev);
}

}  // namespace

PE_TEST(canvasview_brush_on_an_unpaintable_layer_says_so) {
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_CHECK(doc != nullptr);

    // An adjustment layer is not paintable pixels, so the stroke cannot begin.
    auto adj = std::make_unique<pe::AdjustmentLayer>(std::make_unique<pe::Invert>(), "Invert");
    const pe::LayerId adjId = adj->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(adj));
    doc->setActiveLayer(adjId);

    pe::app::CanvasView view;
    view.resize(200, 200);
    view.setDocument(doc.get());
    view.setTool(pe::app::CanvasView::Tool::Brush);

    const QStringList said = messagesFrom(view, [&] { pressLeft(view); });
    PE_CHECK_EQ(said.size(), 1);
    PE_CHECK(said.value(0).contains(QStringLiteral("pixel layer")));

    view.setDocument(nullptr);
}

PE_TEST(canvasview_brush_with_no_active_layer_says_so) {
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_CHECK(doc != nullptr);
    doc->setActiveLayer(pe::kNoLayer);

    pe::app::CanvasView view;
    view.resize(200, 200);
    view.setDocument(doc.get());
    view.setTool(pe::app::CanvasView::Tool::Brush);

    const QStringList said = messagesFrom(view, [&] { pressLeft(view); });
    PE_CHECK_EQ(said.size(), 1);
    PE_CHECK(said.value(0).contains(QStringLiteral("layer")));

    view.setDocument(nullptr);
}

PE_TEST(canvasview_brush_on_a_pixel_layer_stays_quiet) {
    // The guard against a message on every stroke: a press that really paints must
    // say nothing at all, or the status bar would nag through normal use.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_CHECK(doc != nullptr);

    pe::app::CanvasView view;
    view.resize(200, 200);
    view.setDocument(doc.get());
    view.setTool(pe::app::CanvasView::Tool::Brush);

    const QStringList said = messagesFrom(view, [&] { pressLeft(view); });
    PE_CHECK_EQ(said.size(), 0);
    PE_CHECK(view.tool().isStroking());  // the stroke really did begin

    view.tool().cancel(*doc);
    view.setDocument(nullptr);
}
