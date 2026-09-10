// The Swatches dock used to hold a centred label reading "Swatches" and nothing else, which
// is indistinguishable from a panel that failed to load. These pin what the real one does.

#include "CanvasView.hpp"
#include "MainWindow.hpp"
#include "SwatchesPanel.hpp"
#include "pe_test.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QList>
#include <QMouseEvent>
#include <QPoint>
#include <QString>
#include <QVector>

#include <cmath>
#include <cstddef>

namespace {

void clickAt(pe::app::SwatchesPanel& p, QPoint at) {
    const QPointF f(at);
    QMouseEvent press(QEvent::MouseButtonPress, f, f, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(&p, &press);
}

void pressKey(pe::app::SwatchesPanel& p, int key) {
    QKeyEvent e(QEvent::KeyPress, key, Qt::NoModifier);
    QCoreApplication::sendEvent(&p, &e);
}

}  // namespace

PE_TEST(swatches_clicking_a_chip_reports_that_chip_s_colour) {
    pe::app::SwatchesPanel panel;
    panel.resize(200, 200);
    PE_REQUIRE(!panel.colors().isEmpty());

    QVector<QColor> heard;
    QObject::connect(&panel, &pe::app::SwatchesPanel::colorChosen,
                     [&heard](const QColor& c) { heard.append(c); });

    // Not "some colour changed": the colour of the chip that was actually under the cursor.
    for (const int index : {0, 3, 11}) {
        const QRect r = QRect();  // chipRect is private; chipAt is the public contract
        (void)r;
        // Walk the widget to find a point that maps to `index`, then click exactly there.
        QPoint hit(-1, -1);
        for (int y = 0; y < panel.height() && hit.x() < 0; ++y) {
            for (int x = 0; x < panel.width(); ++x) {
                if (panel.chipAt(QPoint(x, y)) == index) {
                    hit = QPoint(x, y);
                    break;
                }
            }
        }
        PE_REQUIRE(hit.x() >= 0);
        const int before = heard.size();
        clickAt(panel, hit);
        PE_REQUIRE(heard.size() == before + 1);
        PE_CHECK(heard.last() == panel.colors()[index]);
        PE_CHECK_EQ(panel.currentIndex(), index);
    }
}

PE_TEST(swatches_a_click_on_the_gap_between_chips_chooses_nothing) {
    // The grid has gaps and a margin. Snapping a near-miss to the closest chip would hand
    // the user a colour they did not point at, which is worse than nothing happening.
    pe::app::SwatchesPanel panel;
    panel.resize(200, 200);

    int heard = 0;
    QObject::connect(&panel, &pe::app::SwatchesPanel::colorChosen,
                     [&heard](const QColor&) { ++heard; });

    PE_CHECK_EQ(panel.chipAt(QPoint(0, 0)), -1);  // the margin
    clickAt(panel, QPoint(0, 0));
    PE_CHECK_EQ(heard, 0);

    // Far below the last row.
    clickAt(panel, QPoint(panel.width() / 2, panel.height() - 1));
    PE_CHECK_EQ(heard, 0);
}

PE_TEST(swatches_the_grid_marks_the_current_colour_without_announcing_it) {
    // MainWindow pushes the foreground in from the picker and the eyedropper. Echoing that
    // back as a choice would loop the two panels against each other.
    pe::app::SwatchesPanel panel;
    panel.resize(200, 200);
    int heard = 0;
    QObject::connect(&panel, &pe::app::SwatchesPanel::colorChosen,
                     [&heard](const QColor&) { ++heard; });

    PE_CHECK_EQ(panel.currentIndex(), -1);  // nothing marked until a colour matches
    panel.setCurrentColor(panel.colors()[5]);
    PE_CHECK_EQ(panel.currentIndex(), 5);
    PE_CHECK_EQ(heard, 0);

    // A colour that is not in the palette clears the mark rather than leaving a stale one.
    panel.setCurrentColor(QColor(1, 2, 3));
    PE_CHECK_EQ(panel.currentIndex(), -1);
    PE_CHECK_EQ(heard, 0);
}

PE_TEST(swatches_can_be_used_from_the_keyboard) {
    // A grid of coloured squares is exactly the control that ends up mouse-only.
    pe::app::SwatchesPanel panel;
    panel.resize(200, 200);
    QVector<QColor> heard;
    QObject::connect(&panel, &pe::app::SwatchesPanel::colorChosen,
                     [&heard](const QColor& c) { heard.append(c); });

    // It has to be able to HOLD focus, or the key handling below is unreachable in the real
    // window however well it works when a test posts events straight at it.
    PE_CHECK(panel.focusPolicy() != Qt::NoFocus);

    pressKey(panel, Qt::Key_Right);  // from nothing selected, this lands on the first chip
    PE_CHECK_EQ(panel.currentIndex(), 0);
    pressKey(panel, Qt::Key_Right);
    PE_CHECK_EQ(panel.currentIndex(), 1);
    pressKey(panel, Qt::Key_Left);
    PE_CHECK_EQ(panel.currentIndex(), 0);
    pressKey(panel, Qt::Key_Left);  // at the edge: stays put rather than wrapping round
    PE_CHECK_EQ(panel.currentIndex(), 0);

    pressKey(panel, Qt::Key_End);
    PE_CHECK_EQ(panel.currentIndex(), static_cast<int>(panel.colors().size()) - 1);
    pressKey(panel, Qt::Key_Home);
    PE_CHECK_EQ(panel.currentIndex(), 0);

    PE_CHECK(!heard.isEmpty());
    PE_CHECK(heard.last() == panel.colors()[0]);
}

PE_TEST(mainwindow_choosing_a_swatch_sets_the_foreground_everywhere) {
    // The point of the panel. A palette that marks a chip and does not load the colour is
    // the same class of defect as the placeholder it replaced.
    pe::app::MainWindow w;
    auto* swatches = w.findChild<pe::app::SwatchesPanel*>(QStringLiteral("SwatchesPanel"));
    PE_REQUIRE(swatches != nullptr);
    PE_REQUIRE(!swatches->colors().isEmpty());

    const QColor want = swatches->colors()[9];
    PE_REQUIRE(want.isValid());
    emit swatches->colorChosen(want);

    // The brush is what actually paints, so that is what has to have changed.
    const pe::Rgbaf ink = w.canvas()->tool().color();
    PE_CHECK(std::abs(ink.r - static_cast<float>(want.redF())) < 0.01f);
    PE_CHECK(std::abs(ink.g - static_cast<float>(want.greenF())) < 0.01f);
    PE_CHECK(std::abs(ink.b - static_cast<float>(want.blueF())) < 0.01f);
    // And the grid marks it, so the two panels agree about what is loaded.
    PE_CHECK_EQ(swatches->currentIndex(), 9);
}

PE_TEST(mainwindow_an_unbuilt_panel_says_what_it_will_be_and_that_it_is_not_built) {
    // Each of these showed its own name centred in a blank dock, which reads as a panel that
    // failed to load rather than one that does not exist yet. The user reported them as
    // simply "empty".
    pe::app::MainWindow w;
    const QList<QWidget*> placeholders =
        w.findChildren<QWidget*>(QStringLiteral("PanelPlaceholder"));
    // Patterns, Libraries, Paths. One fewer each time a panel becomes real, so this number
    // going down is the point; it is here so the loop below cannot pass by finding nothing.
    PE_CHECK(placeholders.size() >= 3);

    for (QWidget* p : placeholders) {
        PE_CHECK(!p->accessibleName().isEmpty());
        PE_CHECK(p->accessibleDescription() == QStringLiteral("Not yet implemented"));
        const QList<QLabel*> labels = p->findChildren<QLabel*>();
        PE_CHECK_EQ(labels.size(), 3);  // its name, what it is for, and that it is not built
        // And all three are actually LAID OUT. A label parented to the panel but never added
        // to its layout is found by findChildren and is not on screen.
        QLayout* lay = p->layout();
        PE_REQUIRE(lay != nullptr);
        int shown = 0;
        for (int i = 0; i < lay->count(); ++i) {
            if (lay->itemAt(i)->widget() != nullptr) ++shown;
        }
        PE_CHECK_EQ(shown, 3);
        bool saidNotImplemented = false;
        bool saidSomethingSubstantial = false;
        for (QLabel* l : labels) {
            if (l->text() == QStringLiteral("Not yet implemented")) saidNotImplemented = true;
            if (l->text().size() > 30) saidSomethingSubstantial = true;
        }
        PE_CHECK(saidNotImplemented);
        PE_CHECK(saidSomethingSubstantial);
    }
}
