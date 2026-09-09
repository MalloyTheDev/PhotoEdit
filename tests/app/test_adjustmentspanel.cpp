// The Adjustments dock used to hold a centred label reading "Adjustments" and nothing else.
// These pin what the real one does, and in particular that a preset is not merely a name: the
// worst outcome here is a panel full of rows that add layers which change nothing, which is
// exactly what Layer > New Adjustment Layer already did.

#include "AdjustmentsPanel.hpp"
#include "MainWindow.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDockWidget>
#include <QEvent>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QList>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QString>

#include <cmath>
#include <cstddef>
#include <memory>

namespace {

std::unique_ptr<pe::Document> docWithPixelLayer() {
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, 64, 64}, pe::Rgba8{120, 90, 60, 255});
    return doc;
}

// Mean of (r+g+b)/3 over the whole image. Enough to say "this got brighter", which is the
// claim a preset called Lighten makes.
double meanLuma(const QImage& img) {
    if (img.isNull()) return -1.0;
    double sum = 0.0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            sum += (c.red() + c.green() + c.blue()) / 3.0;
        }
    }
    return sum / (img.width() * img.height());
}

int presetNamed(const pe::app::AdjustmentsPanel& p, const QString& name) {
    for (int i = 0; i < p.presetCount(); ++i) {
        if (p.preset(i).name == name) return i;
    }
    return -1;
}

QListWidget* listOf(pe::app::AdjustmentsPanel& p) {
    return p.findChild<QListWidget*>();
}

// The row carrying preset `index`, or -1. Rows and presets do not line up: every group
// contributes a heading row of its own.
int rowForPreset(QListWidget* list, int index) {
    for (int r = 0; r < list->count(); ++r) {
        const QVariant v = list->item(r)->data(Qt::UserRole);
        if (v.isValid() && v.toInt() == index) return r;
    }
    return -1;
}

void clickRow(QListWidget* list, int row) {
    const QPointF at(list->visualItemRect(list->item(row)).center());
    QMouseEvent press(QEvent::MouseButtonPress, at, at, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, at, at, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(list->viewport(), &press);
    QCoreApplication::sendEvent(list->viewport(), &release);
}

}  // namespace

PE_TEST(adjustments_every_preset_actually_changes_the_image) {
    // The whole point of the panel. An adjustment layer built at its identity settings is
    // indistinguishable from no layer at all, and that is what the menu entries produce; a
    // preset that shipped with, say, gamma left at 1.0 would look like a working row and do
    // nothing. The engine itself answers here: the preview IS the adjustment applied.
    pe::app::AdjustmentsPanel panel;
    PE_REQUIRE(panel.presetCount() > 0);

    const QImage reference = pe::app::AdjustmentsPanel::referenceStrip();
    PE_REQUIRE(!reference.isNull());

    for (int i = 0; i < panel.presetCount(); ++i) {
        const QImage got = panel.preview(i);
        PE_CHECK(!got.isNull());
        if (got == reference) {
            std::printf("      preset %d (%s) changes nothing\n", i,
                        panel.preset(i).name.toUtf8().constData());
        }
        PE_CHECK(got != reference);
    }
}

PE_TEST(adjustments_a_preset_is_built_from_the_adjustment_it_names) {
    // The row says "Levels" in its tooltip; it had better not build a Curves. Nothing else
    // would catch a copy-paste in the table, and the tooltip is the only thing telling the
    // user what the layer they are about to add actually is.
    pe::app::AdjustmentsPanel panel;
    for (int i = 0; i < panel.presetCount(); ++i) {
        const std::unique_ptr<pe::Adjustment> adj = panel.makeAdjustment(i);
        PE_REQUIRE(adj != nullptr);
        const QString actual = QString::fromStdString(adj->name());
        if (actual != panel.preset(i).type) {
            std::printf("      preset %d (%s) says %s, builds %s\n", i,
                        panel.preset(i).name.toUtf8().constData(),
                        panel.preset(i).type.toUtf8().constData(), actual.toUtf8().constData());
        }
        PE_CHECK(actual == panel.preset(i).type);
    }
}

PE_TEST(adjustments_lighten_and_darken_move_the_image_the_way_they_are_named) {
    // Not just "differs from the reference": a preset called Lighten that darkens would pass
    // that. Direction is the claim the name makes.
    pe::app::AdjustmentsPanel panel;
    const int lighten = presetNamed(panel, QStringLiteral("Lighten"));
    const int darken = presetNamed(panel, QStringLiteral("Darken"));
    const int plus = presetNamed(panel, QStringLiteral("Exposure +1 Stop"));
    const int minus = presetNamed(panel, QStringLiteral("Exposure -1 Stop"));
    PE_REQUIRE(lighten >= 0 && darken >= 0 && plus >= 0 && minus >= 0);

    const double base = meanLuma(pe::app::AdjustmentsPanel::referenceStrip());
    PE_CHECK(meanLuma(panel.preview(lighten)) > base);
    PE_CHECK(meanLuma(panel.preview(darken)) < base);
    PE_CHECK(meanLuma(panel.preview(plus)) > base);
    PE_CHECK(meanLuma(panel.preview(minus)) < base);
}

PE_TEST(adjustments_the_monochrome_presets_leave_no_colour_behind) {
    // A "Black & White" row that still shows a hue sweep in its swatch is telling the user
    // something false about the layer it adds. The reference strip is half hue sweep, so
    // there is real colour for it to fail to remove.
    pe::app::AdjustmentsPanel panel;
    for (int i = 0; i < panel.presetCount(); ++i) {
        if (panel.preset(i).group != QStringLiteral("Monochrome")) continue;
        if (panel.preset(i).type != QStringLiteral("Black & White")) continue;  // duotone is inked
        const QImage img = panel.preview(i);
        PE_REQUIRE(!img.isNull());
        int coloured = 0;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const QColor c = img.pixelColor(x, y);
                // One 8-bit step of rounding slack; anything wider is a surviving hue.
                if (std::abs(c.red() - c.green()) > 1 || std::abs(c.green() - c.blue()) > 1) {
                    ++coloured;
                }
            }
        }
        if (coloured != 0) {
            std::printf("      preset %s left %d coloured pixels\n",
                        panel.preset(i).name.toUtf8().constData(), coloured);
        }
        PE_CHECK_EQ(coloured, 0);
    }
}

PE_TEST(adjustments_every_preset_has_its_own_name_and_says_what_it_does) {
    // The preset's name becomes the LAYER's name, so two presets sharing one would produce a
    // stack where the rows cannot be told apart. An empty description leaves a row with no
    // tooltip, which is the state the whole panel exists to get out of.
    pe::app::AdjustmentsPanel panel;
    PE_REQUIRE(panel.presetCount() > 0);
    for (int i = 0; i < panel.presetCount(); ++i) {
        PE_CHECK(!panel.preset(i).name.isEmpty());
        PE_CHECK(!panel.preset(i).group.isEmpty());
        PE_CHECK(panel.preset(i).description.size() > 20);
        for (int j = i + 1; j < panel.presetCount(); ++j) {
            if (panel.preset(i).name == panel.preset(j).name) {
                std::printf("      presets %d and %d are both named %s\n", i, j,
                            panel.preset(i).name.toUtf8().constData());
            }
            PE_CHECK(panel.preset(i).name != panel.preset(j).name);
        }
    }
}

PE_TEST(adjustments_an_out_of_range_index_yields_nothing_rather_than_reading_off_the_end) {
    pe::app::AdjustmentsPanel panel;
    PE_CHECK(panel.makeAdjustment(-1) == nullptr);
    PE_CHECK(panel.makeAdjustment(panel.presetCount()) == nullptr);
    PE_CHECK(panel.preview(-1).isNull());
    PE_CHECK(panel.preview(panel.presetCount()).isNull());

    int heard = 0;
    QObject::connect(&panel, &pe::app::AdjustmentsPanel::presetChosen, [&heard](int) { ++heard; });
    panel.activate(-1);
    panel.activate(panel.presetCount());
    PE_CHECK_EQ(heard, 0);
}

PE_TEST(adjustments_clicking_a_row_chooses_that_row_s_preset) {
    pe::app::AdjustmentsPanel panel;
    panel.resize(240, 600);
    QListWidget* list = listOf(panel);
    PE_REQUIRE(list != nullptr);
    PE_REQUIRE(list->count() > panel.presetCount());  // every group added a heading row

    int heard = -1;
    QObject::connect(&panel, &pe::app::AdjustmentsPanel::presetChosen,
                     [&heard](int i) { heard = i; });

    // Not "some preset was chosen": the one whose row was actually under the cursor.
    for (const int index : {0, 1, panel.presetCount() - 1}) {
        const int row = rowForPreset(list, index);
        PE_REQUIRE(row >= 0);
        list->scrollToItem(list->item(row));
        heard = -1;
        clickRow(list, row);
        PE_CHECK_EQ(heard, index);
    }
}

PE_TEST(adjustments_a_group_heading_is_not_a_choice) {
    // Headings are rows in the same list. If they were selectable they would look like
    // presets, and clicking "Tone" would either do nothing visible or add a wrong layer.
    pe::app::AdjustmentsPanel panel;
    panel.resize(240, 600);
    QListWidget* list = listOf(panel);
    PE_REQUIRE(list != nullptr);

    int headings = 0;
    int heard = 0;
    QObject::connect(&panel, &pe::app::AdjustmentsPanel::presetChosen, [&heard](int) { ++heard; });
    for (int r = 0; r < list->count(); ++r) {
        if (list->item(r)->data(Qt::UserRole).isValid()) continue;
        ++headings;
        PE_CHECK(list->item(r)->flags() == Qt::NoItemFlags);
        list->scrollToItem(list->item(r));
        clickRow(list, r);
    }
    PE_CHECK(headings >= 2);  // there is more than one group
    PE_CHECK_EQ(heard, 0);
}

PE_TEST(adjustments_return_on_the_current_row_chooses_it) {
    // Reachable without a mouse. A list of rows is exactly the sort of control that ends up
    // mouse-only, and QListWidget's own itemActivated fires on a single click under some
    // styles, which would add two layers per click.
    pe::app::AdjustmentsPanel panel;
    QListWidget* list = listOf(panel);
    PE_REQUIRE(list != nullptr);
    const int index = panel.presetCount() / 2;
    const int row = rowForPreset(list, index);
    PE_REQUIRE(row >= 0);
    list->setCurrentRow(row);

    int heard = -1;
    int count = 0;
    QObject::connect(&panel, &pe::app::AdjustmentsPanel::presetChosen, [&](int i) {
        heard = i;
        ++count;
    });
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(list, &key);
    PE_CHECK_EQ(heard, index);
    PE_CHECK_EQ(count, 1);
}

PE_TEST(adjustments_choosing_a_preset_adds_one_undoable_adjustment_layer) {
    // The end-to-end path the user takes: a row in the dock becomes a layer in the document,
    // as one history entry, named after the preset rather than after its adjustment type.
    pe::app::MainWindow w;
    w.setDocument(docWithPixelLayer(), QString());
    auto* panel = w.findChild<pe::app::AdjustmentsPanel*>();
    PE_REQUIRE(panel != nullptr);

    const int index = presetNamed(*panel, QStringLiteral("Sepia"));
    PE_REQUIRE(index >= 0);
    const std::size_t layersBefore = w.document()->topLevelCount();
    const std::size_t undoBefore = w.document()->history().undoDepth();

    panel->activate(index);

    PE_CHECK_EQ(w.document()->topLevelCount(), layersBefore + 1);
    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore + 1);

    const pe::Layer* added = w.document()->findLayer(w.document()->activeLayer());
    PE_REQUIRE(added != nullptr);
    PE_CHECK(added->isAdjustment());
    // Named for the look, not the machinery: "Sepia" is what the user asked for, and
    // "Hue/Saturation" would tell them nothing about which of two colorize layers is which.
    PE_CHECK(QString::fromStdString(added->name()) == QStringLiteral("Sepia"));
    const auto* adjLayer = static_cast<const pe::AdjustmentLayer*>(added);
    PE_CHECK(adjLayer->adjustment().kind() == pe::AdjustmentKind::HueSaturation);
    // On top of the stack, which is what the panel's own hint says happens.
    PE_CHECK_EQ(w.document()->topLevelIndexOf(added->id()), layersBefore);

    w.document()->history().undo();
    PE_CHECK_EQ(w.document()->topLevelCount(), layersBefore);
}

PE_TEST(adjustments_the_dock_holds_the_real_panel_and_the_panel_follows_the_document) {
    pe::app::MainWindow w;
    const QList<QDockWidget*> docks = w.findChildren<QDockWidget*>();
    QDockWidget* dock = nullptr;
    for (QDockWidget* d : docks) {
        if (d->objectName() == QStringLiteral("Adjustments")) dock = d;
    }
    PE_REQUIRE(dock != nullptr);
    // Not a placeholder label: the dock's own widget is the panel.
    PE_CHECK(qobject_cast<pe::app::AdjustmentsPanel*>(dock->widget()) != nullptr);

    auto* panel = w.findChild<pe::app::AdjustmentsPanel*>();
    PE_REQUIRE(panel != nullptr);
    w.setDocument(docWithPixelLayer(), QString());
    PE_CHECK(panel->isEnabled());
    // Every preset adds a layer to a document. With none open the rows must not look live.
    w.setDocument(nullptr, QString());
    PE_CHECK(!panel->isEnabled());
}

PE_TEST(adjustments_the_panel_says_where_the_layer_lands) {
    // The one thing a user cannot discover by looking: which end of the stack it goes on.
    pe::app::AdjustmentsPanel panel;
    const QLabel* hint = nullptr;
    for (const QLabel* l : panel.findChildren<QLabel*>()) {
        if (l->text().contains(QStringLiteral("top of the stack"))) hint = l;
    }
    PE_REQUIRE(hint != nullptr);
    // The object name is the whole of the contract with the stylesheet: the theme dims this
    // label and clears its background through QLabel#PanelHint, and a renamed label silently
    // reverts to full-strength text on whatever ground it inherits.
    PE_CHECK(hint->objectName() == QStringLiteral("PanelHint"));
}
