// The Gradients dock used to hold a centred label, over a tool that could draw exactly one
// ramp. These pin that a swatch shows the ramp it is labelled with, that the two presets which
// follow the loaded colours actually do, and that choosing one reaches the tool.

#include "CanvasView.hpp"
#include "ColorPanel.hpp"
#include "GradientsPanel.hpp"
#include "MainWindow.hpp"
#include "pe/core/Color.hpp"
#include "pe/core/Gradient.hpp"
#include "pe_test.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDockWidget>
#include <QEvent>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QList>
#include <QListWidget>
#include <QListWidgetItem>
#include <QSize>
#include <QString>

#include <algorithm>
#include <cmath>

namespace {

QListWidget* listOf(pe::app::GradientsPanel& p) {
    return p.findChild<QListWidget*>();
}

int presetNamed(const pe::app::GradientsPanel& p, const QString& name) {
    for (int i = 0; i < p.presetCount(); ++i) {
        if (p.preset(i).name == name) return i;
    }
    return -1;
}

}  // namespace

PE_TEST(gradients_the_dock_holds_the_real_panel) {
    pe::app::MainWindow w;
    QDockWidget* dock = nullptr;
    for (QDockWidget* d : w.findChildren<QDockWidget*>()) {
        if (d->objectName() == QStringLiteral("Gradients")) dock = d;
    }
    PE_REQUIRE(dock != nullptr);
    PE_CHECK(qobject_cast<pe::app::GradientsPanel*>(dock->widget()) != nullptr);
}

PE_TEST(gradients_every_preset_is_a_real_ramp_and_not_a_flat_colour) {
    // The failure this exists to catch: a list of rows whose swatches all look the same, or a
    // preset whose stops collapsed so it draws one colour. A gradient that does not change
    // across its own swatch is not a gradient.
    pe::app::GradientsPanel panel;
    PE_REQUIRE(panel.presetCount() >= 6);
    for (int i = 0; i < panel.presetCount(); ++i) {
        const QImage img = panel.swatch(i, QSize(96, 8));
        PE_REQUIRE(!img.isNull());
        // The widest departure from the starting colour anywhere along the ramp, not the
        // difference between the two ends: Spectrum is a full hue wheel and comes back to red,
        // so end-to-end it looks flat while being the most colourful ramp in the list.
        const QColor left = img.pixelColor(0, 4);
        int spread = 0;
        for (int x = 1; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, 4);
            const int d = std::abs(left.red() - c.red()) + std::abs(left.green() - c.green()) +
                          std::abs(left.blue() - c.blue());
            spread = std::max(spread, d);
        }
        if (spread <= 20) {
            std::printf("      preset %d (%s) barely changes across its swatch\n", i,
                        panel.preset(i).name.toUtf8().constData());
        }
        PE_CHECK(spread > 20);
    }
}

PE_TEST(gradients_a_middle_stop_shows_in_the_swatch) {
    // A multi-stop ramp drawn as a straight fade between its ends would look plausible and be
    // wrong. Copper has a bright band off-centre, which a two-stop reading cannot produce.
    pe::app::GradientsPanel panel;
    const int copper = presetNamed(panel, QStringLiteral("Copper"));
    PE_REQUIRE(copper >= 0);
    const QImage img = panel.swatch(copper, QSize(96, 4));
    PE_REQUIRE(!img.isNull());

    const auto luma = [&img](int x) {
        const QColor c = img.pixelColor(x, 2);
        return (c.red() + c.green() + c.blue()) / 3;
    };
    // Brighter in the middle than at either end: that is the highlight, and it is what a
    // straight interpolation between the two dark ends could never give.
    const int middle = luma(50);
    PE_CHECK(middle > luma(2) + 40);
    PE_CHECK(middle > luma(93) + 40);
}

PE_TEST(gradients_the_dynamic_presets_follow_the_loaded_colours) {
    // The reason stops carry a source rather than a colour: one preset, whatever is loaded.
    pe::app::GradientsPanel panel;
    const int fgToBg = presetNamed(panel, QStringLiteral("Foreground to Background"));
    PE_REQUIRE(fgToBg >= 0);

    panel.setColors(QColor(255, 0, 0), QColor(0, 0, 255));
    QImage img = panel.swatch(fgToBg, QSize(64, 4));
    PE_CHECK(img.pixelColor(0, 2).red() > 200);
    PE_CHECK(img.pixelColor(63, 2).blue() > 200);

    // Load different colours and the SAME preset draws differently.
    panel.setColors(QColor(0, 255, 0), QColor(255, 255, 0));
    img = panel.swatch(fgToBg, QSize(64, 4));
    PE_CHECK(img.pixelColor(0, 2).green() > 200);
    PE_CHECK(img.pixelColor(0, 2).red() < 60);
    PE_CHECK(img.pixelColor(63, 2).red() > 200);
    PE_CHECK(img.pixelColor(63, 2).green() > 200);
}

PE_TEST(gradients_a_fixed_preset_ignores_the_loaded_colours) {
    // The other half of the same claim. If every preset tracked the foreground, the named
    // ramps would all turn into it.
    pe::app::GradientsPanel panel;
    const int sunset = presetNamed(panel, QStringLiteral("Sunset"));
    PE_REQUIRE(sunset >= 0);
    panel.setColors(QColor(255, 0, 0), QColor(0, 0, 255));
    const QImage before = panel.swatch(sunset, QSize(64, 4));
    panel.setColors(QColor(0, 255, 0), QColor(255, 255, 255));
    const QImage after = panel.swatch(sunset, QSize(64, 4));
    PE_CHECK(before == after);
}

PE_TEST(gradients_a_transparent_stop_shows_the_checkerboard_through_it) {
    // Without this the fading presets are indistinguishable from fading-to-the-panel-colour,
    // which is a different gradient and would draw differently on the canvas.
    pe::app::GradientsPanel panel;
    const int fade = presetNamed(panel, QStringLiteral("White to Transparent"));
    PE_REQUIRE(fade >= 0);
    const QImage img = panel.swatch(fade, QSize(64, 20));
    PE_REQUIRE(!img.isNull());
    // At the transparent end the checkerboard alternates down the column; at the opaque end it
    // is one flat white.
    bool variedAtFadedEnd = false;
    for (int y = 1; y < 20; ++y) {
        if (img.pixelColor(63, y) != img.pixelColor(63, y - 1)) variedAtFadedEnd = true;
    }
    PE_CHECK(variedAtFadedEnd);
    bool flatAtSolidEnd = true;
    for (int y = 1; y < 20; ++y) {
        if (img.pixelColor(0, y) != img.pixelColor(0, y - 1)) flatAtSolidEnd = false;
    }
    PE_CHECK(flatAtSolidEnd);
}

PE_TEST(gradients_clicking_a_row_chooses_that_ramp) {
    pe::app::GradientsPanel panel;
    QListWidget* list = listOf(panel);
    PE_REQUIRE(list != nullptr);
    PE_CHECK_EQ(list->count(), panel.presetCount());

    int heard = -1;
    int count = 0;
    QObject::connect(&panel, &pe::app::GradientsPanel::gradientChosen, [&](int i) {
        heard = i;
        ++count;
    });

    for (const int index : {2, 0, panel.presetCount() - 1}) {
        emit list->itemClicked(list->item(index));
        PE_CHECK_EQ(heard, index);
        PE_CHECK_EQ(panel.currentIndex(), index);
    }
    PE_CHECK_EQ(count, 3);
}

PE_TEST(gradients_an_out_of_range_index_chooses_nothing) {
    pe::app::GradientsPanel panel;
    int count = 0;
    QObject::connect(&panel, &pe::app::GradientsPanel::gradientChosen, [&count](int) { ++count; });
    panel.activate(-1);
    panel.activate(panel.presetCount());
    PE_CHECK_EQ(count, 0);
    PE_CHECK(panel.swatch(-1, QSize(10, 10)).isNull());
    PE_CHECK(panel.swatch(panel.presetCount(), QSize(10, 10)).isNull());
}

PE_TEST(gradients_return_on_the_current_row_chooses_it) {
    pe::app::GradientsPanel panel;
    QListWidget* list = listOf(panel);
    PE_REQUIRE(list != nullptr);
    list->setCurrentRow(3);

    int heard = -1;
    int count = 0;
    QObject::connect(&panel, &pe::app::GradientsPanel::gradientChosen, [&](int i) {
        heard = i;
        ++count;
    });
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(list, &key);
    PE_CHECK_EQ(heard, 3);
    PE_CHECK_EQ(count, 1);
}

PE_TEST(gradients_the_tool_starts_on_foreground_to_background) {
    // What the Gradient tool drew before there was a panel. Changing that silently would alter
    // every existing muscle memory for the tool.
    pe::app::CanvasView view;
    const pe::Gradient& g = view.gradient();
    PE_CHECK(!g.isFixed());
    const pe::Rgbaf red{1.0f, 0.0f, 0.0f, 1.0f};
    const pe::Rgbaf blue{0.0f, 0.0f, 1.0f, 1.0f};
    PE_CHECK(g.sample(0.0f, red, blue).r > 0.9f);
    PE_CHECK(g.sample(1.0f, red, blue).b > 0.9f);
}

PE_TEST(gradients_choosing_a_ramp_loads_it_into_the_tool) {
    // The end of the path: a row in the dock becomes what the next drag draws.
    pe::app::MainWindow w;
    auto* panel = w.findChild<pe::app::GradientsPanel*>();
    PE_REQUIRE(panel != nullptr);
    const int spectrum = presetNamed(*panel, QStringLiteral("Spectrum"));
    PE_REQUIRE(spectrum >= 0);

    panel->activate(spectrum);

    const pe::Gradient& loaded = w.canvas()->gradient();
    PE_CHECK(loaded == panel->preset(spectrum).gradient);
    PE_CHECK(loaded.stops().size() > 2);  // and it is the multi-stop one, not a two-stop stand-in
}

PE_TEST(gradients_the_swatches_follow_a_colour_picked_anywhere_in_the_window) {
    // The panel is not wired to the picker directly; it hangs off the one place every colour
    // change ends up, so the eyedropper and the colour dialog reach it too.
    pe::app::MainWindow w;
    auto* panel = w.findChild<pe::app::GradientsPanel*>();
    PE_REQUIRE(panel != nullptr);
    const int fgToBg = presetNamed(*panel, QStringLiteral("Foreground to Background"));
    PE_REQUIRE(fgToBg >= 0);

    // Through the Color panel, which is a path a user actually takes, rather than through the
    // private setter it happens to funnel into.
    auto* picker = w.findChild<pe::app::ColorPanel*>();
    PE_REQUIRE(picker != nullptr);
    emit picker->colorChanged(QColor(255, 0, 0));
    const QImage red = panel->swatch(fgToBg, QSize(32, 4));
    emit picker->colorChanged(QColor(0, 255, 0));
    const QImage green = panel->swatch(fgToBg, QSize(32, 4));

    PE_CHECK(red.pixelColor(0, 2).red() > 200);
    PE_CHECK(green.pixelColor(0, 2).green() > 200);
    PE_CHECK(red != green);
}

PE_TEST(gradients_the_panel_says_what_it_cannot_do_yet) {
    pe::app::GradientsPanel panel;
    bool told = false;
    for (const QObject* child : panel.children()) {
        const QString text = child->property("text").toString();
        if (text.contains(QStringLiteral("not implemented"))) told = true;
    }
    PE_CHECK(told);
}

PE_TEST(gradients_only_the_rows_that_can_change_are_redrawn) {
    // The row ICONS, not just what swatch() computes on demand. A colour pick emits on every
    // drag of the picker, so redrawing all nine swatches on each one is waste; redrawing none
    // of them leaves the two dynamic rows showing the previous colours, which is worse.
    pe::app::GradientsPanel panel;
    QListWidget* list = listOf(panel);
    PE_REQUIRE(list != nullptr);
    const int dynamic = presetNamed(panel, QStringLiteral("Foreground to Background"));
    const int fixed = presetNamed(panel, QStringLiteral("Sunset"));
    PE_REQUIRE(dynamic >= 0 && fixed >= 0);

    panel.setColors(QColor(255, 0, 0), QColor(0, 0, 255));
    const QImage dynBefore = list->item(dynamic)->icon().pixmap(96, 20).toImage();
    const QImage fixBefore = list->item(fixed)->icon().pixmap(96, 20).toImage();

    panel.setColors(QColor(0, 255, 0), QColor(255, 255, 0));
    const QImage dynAfter = list->item(dynamic)->icon().pixmap(96, 20).toImage();
    const QImage fixAfter = list->item(fixed)->icon().pixmap(96, 20).toImage();

    PE_CHECK(dynBefore != dynAfter);  // it follows the colours, so its row must too
    PE_CHECK(fixBefore == fixAfter);  // and this one cannot change, so it is not redrawn
}
