// The Channels dock used to hold a centred label reading "Channels" and nothing else. These
// pin what the real one does, and above all that a row's thumbnail shows the channel it is
// labelled with: four rows that all show the composite would look finished and say nothing.

#include "CanvasView.hpp"
#include "ChannelsPanel.hpp"
#include "MainWindow.hpp"
#include "pe/core/Channels.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Selection.hpp"
#include "pe/core/Tile.hpp"
#include "pe_test.hpp"

#include <QColor>
#include <QCoreApplication>
#include <QDockWidget>
#include <QEvent>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QList>
#include <QPixmap>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include <cstddef>
#include <memory>

namespace {

constexpr int kEyeColumn = 0;
constexpr int kNameColumn = 1;

std::unique_ptr<pe::Document> docWithColour(pe::Rgba8 c) {
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    pl->tiles().fillRect(pe::Rect{0, 0, 64, 64}, c);
    return doc;
}

QTreeWidget* treeOf(pe::app::ChannelsPanel& p) {
    return p.findChild<QTreeWidget*>();
}

// The mean grey of a row's thumbnail, over the pixels that are not the letterbox ground.
// Reading the icon back is the only way to assert a row shows what it says it does.
double meanOf(const QIcon& icon, bool& sawColour) {
    // At its natural size, and only the middle of it: the thumbnail letterboxes the image
    // over the theme's base colour, which is a tinted grey, so the surround is neither the
    // channel nor neutral and would drag every reading toward it.
    const QImage img = icon.pixmap(44, 30).toImage();
    const int x0 = img.width() / 4;
    const int x1 = img.width() - x0;
    const int y0 = img.height() / 4;
    const int y1 = img.height() - y0;
    double sum = 0.0;
    int n = 0;
    sawColour = false;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const QColor px = img.pixelColor(x, y);
            if (px.red() != px.green() || px.green() != px.blue()) sawColour = true;
            sum += (px.red() + px.green() + px.blue()) / 3.0;
            ++n;
        }
    }
    return n > 0 ? sum / n : -1.0;
}

void clickRow(QTreeWidget* tree, int row, int column) {
    // itemClicked is what a real click reaches the panel through; emitting it directly keeps
    // the test off the scroll position and the row geometry, which are Qt's business.
    emit tree->itemClicked(tree->topLevelItem(row), column);
}

}  // namespace

PE_TEST(channels_the_dock_holds_the_real_panel) {
    pe::app::MainWindow w;
    QDockWidget* dock = nullptr;
    for (QDockWidget* d : w.findChildren<QDockWidget*>()) {
        if (d->objectName() == QStringLiteral("Channels")) dock = d;
    }
    PE_REQUIRE(dock != nullptr);
    PE_CHECK(qobject_cast<pe::app::ChannelsPanel*>(dock->widget()) != nullptr);
}

PE_TEST(channels_there_is_a_row_for_the_composite_and_one_per_colour_plane) {
    pe::app::ChannelsPanel panel;
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);
    PE_REQUIRE(tree->topLevelItemCount() == 4);
    PE_CHECK(tree->topLevelItem(0)->text(kNameColumn) == QStringLiteral("RGB"));
    PE_CHECK(tree->topLevelItem(1)->text(kNameColumn) == QStringLiteral("Red"));
    PE_CHECK(tree->topLevelItem(2)->text(kNameColumn) == QStringLiteral("Green"));
    PE_CHECK(tree->topLevelItem(3)->text(kNameColumn) == QStringLiteral("Blue"));
    // Everything visible to start with: the canvas shows the composite until asked otherwise.
    PE_CHECK(panel.view().showsAll());
}

PE_TEST(channels_clicking_a_row_views_that_channel_on_its_own) {
    pe::app::ChannelsPanel panel;
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);

    pe::ChannelView heard{};
    int count = 0;
    QObject::connect(&panel, &pe::app::ChannelsPanel::viewChanged, [&](pe::ChannelView v) {
        heard = v;
        ++count;
    });

    clickRow(tree, 1, kNameColumn);  // Red
    PE_CHECK(heard == (pe::ChannelView{true, false, false}));
    PE_CHECK(panel.view() == heard);
    clickRow(tree, 3, kNameColumn);  // Blue
    PE_CHECK(heard == (pe::ChannelView{false, false, true}));
    clickRow(tree, 0, kNameColumn);  // back to the composite
    PE_CHECK(heard.showsAll());
    PE_CHECK_EQ(count, 3);

    // Clicking the same row again changes nothing, so it must not re-announce: a repeat would
    // repaint the canvas for no reason.
    clickRow(tree, 0, kNameColumn);
    PE_CHECK_EQ(count, 3);
}

PE_TEST(channels_the_eyes_combine_rather_than_soloing) {
    // Hiding blue must leave red and green, in colour. This is the case a radio-button model
    // of channel visibility gets wrong, and it is the one that makes the panel worth having.
    pe::app::ChannelsPanel panel;
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);

    pe::ChannelView heard{};
    QObject::connect(&panel, &pe::app::ChannelsPanel::viewChanged,
                     [&heard](pe::ChannelView v) { heard = v; });

    tree->topLevelItem(3)->setCheckState(kEyeColumn, Qt::Unchecked);  // Blue off
    PE_CHECK(heard == (pe::ChannelView{true, true, false}));
    // And the composite row stops claiming everything is shown.
    PE_CHECK(tree->topLevelItem(0)->checkState(kEyeColumn) == Qt::Unchecked);

    tree->topLevelItem(3)->setCheckState(kEyeColumn, Qt::Checked);  // Blue back
    PE_CHECK(heard.showsAll());
    PE_CHECK(tree->topLevelItem(0)->checkState(kEyeColumn) == Qt::Checked);
}

PE_TEST(channels_the_composite_eye_carries_all_three) {
    pe::app::ChannelsPanel panel;
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);

    pe::ChannelView heard{};
    QObject::connect(&panel, &pe::app::ChannelsPanel::viewChanged,
                     [&heard](pe::ChannelView v) { heard = v; });

    tree->topLevelItem(0)->setCheckState(kEyeColumn, Qt::Unchecked);
    PE_CHECK(heard == (pe::ChannelView{false, false, false}));
    for (int row = 1; row <= 3; ++row) {
        PE_CHECK(tree->topLevelItem(row)->checkState(kEyeColumn) == Qt::Unchecked);
    }
    tree->topLevelItem(0)->setCheckState(kEyeColumn, Qt::Checked);
    PE_CHECK(heard.showsAll());
}

PE_TEST(channels_setting_the_view_from_outside_does_not_echo) {
    // MainWindow pushes the canvas's state in; if that came back out as a request the two
    // would ping-pong.
    pe::app::ChannelsPanel panel;
    int count = 0;
    QObject::connect(&panel, &pe::app::ChannelsPanel::viewChanged,
                     [&count](pe::ChannelView) { ++count; });
    panel.setView(pe::ChannelView{false, true, false});
    PE_CHECK_EQ(count, 0);
    PE_CHECK(panel.view() == (pe::ChannelView{false, true, false}));
    // And the eyes followed it, or the panel would be describing a view it is not in.
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);
    PE_CHECK(tree->topLevelItem(1)->checkState(kEyeColumn) == Qt::Unchecked);
    PE_CHECK(tree->topLevelItem(2)->checkState(kEyeColumn) == Qt::Checked);
}

PE_TEST(channels_each_row_shows_the_plane_it_is_named_after) {
    // The defect this exists to catch: four rows that all draw the composite, or the green
    // row drawing red. A flat colour makes each plane a known constant grey.
    pe::app::ChannelsPanel panel;
    auto doc = docWithColour(pe::Rgba8{200, 120, 40, 255});
    panel.setDocument(doc.get());
    panel.setPreviewSource([](int) {
        pe::PixelBuffer buf(16, 16);
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) buf.set(x, y, pe::Rgba8{200, 120, 40, 255});
        }
        return buf;
    });
    panel.show();  // thumbnails are only built while the dock is on screen
    QCoreApplication::processEvents();

    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);

    bool colour = false;
    const double composite = meanOf(tree->topLevelItem(0)->icon(kNameColumn), colour);
    PE_CHECK(colour);  // the composite row is the picture, in colour

    const double want[3] = {200.0, 120.0, 40.0};
    for (int i = 0; i < 3; ++i) {
        const double got = meanOf(tree->topLevelItem(i + 1)->icon(kNameColumn), colour);
        if (colour || got < want[i] - 12.0 || got > want[i] + 12.0) {
            std::printf("      row %d: mean %.1f (wanted ~%.0f), coloured=%d\n", i + 1, got,
                        want[i], colour ? 1 : 0);
        }
        PE_CHECK(!colour);  // a plane is shown as grey, not tinted
        PE_CHECK(got > want[i] - 12.0);
        PE_CHECK(got < want[i] + 12.0);
    }
    PE_CHECK(composite > 0.0);
    panel.setDocument(nullptr);
}

PE_TEST(channels_thumbnails_are_not_rebuilt_while_the_dock_is_hidden) {
    // The Channels dock shares a tab group with Layers, so it is usually behind it.
    // Recompositing a preview on every stroke of a document nobody is looking at is the
    // panel-refresh waste #156 is about.
    pe::app::ChannelsPanel panel;
    auto doc = docWithColour(pe::Rgba8{10, 20, 30, 255});
    int pulls = 0;
    panel.setPreviewSource([&pulls](int) {
        ++pulls;
        pe::PixelBuffer buf(8, 8);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) buf.set(x, y, pe::Rgba8{10, 20, 30, 255});
        }
        return buf;
    });
    panel.setDocument(doc.get());
    PE_CHECK_EQ(pulls, 0);  // never shown

    auto* pl = static_cast<pe::PixelLayer*>(doc->findLayer(doc->activeLayer()));
    doc->history().push(std::make_unique<pe::AddLayerCommand>(
        std::make_unique<pe::PixelLayer>("Second"), doc->topLevelCount()));
    (void)pl;
    PE_CHECK_EQ(pulls, 0);  // a real edit, and still nothing: the dock is hidden

    panel.show();
    QCoreApplication::processEvents();
    PE_CHECK(pulls >= 1);  // raised, so it catches up
    panel.setDocument(nullptr);
}

PE_TEST(channels_a_selection_change_does_not_rebuild_the_thumbnails) {
    // Selection moves no pixels, so the planes are exactly as they were.
    pe::app::ChannelsPanel panel;
    auto doc = docWithColour(pe::Rgba8{10, 20, 30, 255});
    int pulls = 0;
    panel.setPreviewSource([&pulls](int) {
        ++pulls;
        pe::PixelBuffer buf(8, 8);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) buf.set(x, y, pe::Rgba8{10, 20, 30, 255});
        }
        return buf;
    });
    panel.setDocument(doc.get());
    panel.show();
    QCoreApplication::processEvents();
    const int afterShow = pulls;
    PE_REQUIRE(afterShow >= 1);

    pe::Selection sel;
    sel.selectRect(pe::Rect{0, 0, 10, 10});
    doc->history().push(std::make_unique<pe::SetSelectionCommand>(std::move(sel)));
    PE_CHECK_EQ(pulls, afterShow);
    panel.setDocument(nullptr);
}

PE_TEST(channels_viewing_a_channel_edits_nothing) {
    // Visibility is display state. If a view change reached the document it would land in
    // history, mark the file unsaved, and be undoable, none of which it should be.
    pe::app::MainWindow w;
    w.setDocument(docWithColour(pe::Rgba8{90, 90, 90, 255}), QString());
    auto* panel = w.findChild<pe::app::ChannelsPanel*>();
    PE_REQUIRE(panel != nullptr);

    const std::size_t undoBefore = w.document()->history().undoDepth();
    const bool dirtyBefore = w.document()->isDirty();

    QTreeWidget* tree = panel->findChild<QTreeWidget*>();
    PE_REQUIRE(tree != nullptr);
    clickRow(tree, 2, kNameColumn);  // view Green alone

    PE_CHECK_EQ(w.document()->history().undoDepth(), undoBefore);
    PE_CHECK(w.document()->isDirty() == dirtyBefore);
    // And the canvas is actually in that view: the panel is wired to it, not just to itself.
    PE_CHECK(w.canvas()->channelView() == (pe::ChannelView{false, true, false}));
}

PE_TEST(channels_a_new_document_comes_back_to_the_composite) {
    // A channel view belongs to the picture being looked at. Carrying one across a File >
    // Open would show the new document through the old document's channel with nothing
    // saying why the image is grey.
    pe::app::MainWindow w;
    w.setDocument(docWithColour(pe::Rgba8{90, 90, 90, 255}), QString());
    auto* panel = w.findChild<pe::app::ChannelsPanel*>();
    PE_REQUIRE(panel != nullptr);

    QTreeWidget* tree = panel->findChild<QTreeWidget*>();
    PE_REQUIRE(tree != nullptr);
    clickRow(tree, 1, kNameColumn);
    PE_REQUIRE(w.canvas()->channelView() == (pe::ChannelView{true, false, false}));

    w.setDocument(docWithColour(pe::Rgba8{10, 10, 10, 255}), QString());
    PE_CHECK(panel->view().showsAll());
    PE_CHECK(w.canvas()->channelView().showsAll());
}

PE_TEST(channels_return_on_a_row_views_it) {
    pe::app::ChannelsPanel panel;
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);
    tree->setCurrentItem(tree->topLevelItem(2));

    pe::ChannelView heard{};
    int count = 0;
    QObject::connect(&panel, &pe::app::ChannelsPanel::viewChanged, [&](pe::ChannelView v) {
        heard = v;
        ++count;
    });
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QCoreApplication::sendEvent(tree, &key);
    PE_CHECK(heard == (pe::ChannelView{false, true, false}));
    PE_CHECK_EQ(count, 1);  // not once per style interpretation of "activated"
}

PE_TEST(channels_the_panel_says_what_it_cannot_do_yet) {
    // Spot channels and saved selections are specified (docs/systems/19-channels.md) and not
    // built. A panel that lists three rows and stops, saying nothing, reads as the whole of
    // what channels are.
    pe::app::ChannelsPanel panel;
    bool told = false;
    for (const QObject* child : panel.children()) {
        const QString text = child->property("text").toString();
        if (text.contains(QStringLiteral("Spot channels")) &&
            text.contains(QStringLiteral("not implemented"))) {
            told = true;
        }
    }
    PE_CHECK(told);
}
