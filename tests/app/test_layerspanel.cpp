// The layers panel had no test coverage at all, which is how it came to show NO thumbnails
// on any document over 4 megapixels, and to recomposite every layer's preview whenever one
// layer's properties changed.
//
// The panel is reachable without an event loop: onDocumentChanged is public (it is the
// DocumentObserver override), a real History::push notifies synchronously, and the tree is
// named so findChild can reach it. Icon pixels read back on the offscreen platform, which
// tests/app/test_iconutil.cpp already relies on.
//
// Lifetime, which this file must respect throughout: the pe::Document is declared BEFORE the
// panel so it outlives it, and every case detaches with setDocument(nullptr) before returning.
// The panel observes the document and unregisters in its destructor.

#include "LayersPanel.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Compositor.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe_test.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>

namespace {

constexpr int kThumb = 26;

QTreeWidget* treeOf(pe::app::LayersPanel& panel) {
    return panel.findChild<QTreeWidget*>(QStringLiteral("LayerTree"));
}

QImage iconImage(const QTreeWidgetItem* item, int column) {
    if (item == nullptr) return QImage();
    return item->icon(column).pixmap(QSize(kThumb, kThumb)).toImage();
}

bool nearColor(QColor a, QColor b, int tol = 6) {
    return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol &&
           std::abs(a.blue() - b.blue()) <= tol;
}

// The two checkerboard greys the thumbnail paints under the content. A transparent layer
// still produces a NON-NULL, non-blank icon, so `isNull()` alone proves nothing: a test has
// to look at the pixels.
bool isChecker(QColor c) {
    return nearColor(c, QColor(88, 94, 104)) || nearColor(c, QColor(66, 72, 82));
}

pe::PixelLayer* baseLayer(pe::Document& doc) {
    return static_cast<pe::PixelLayer*>(doc.findLayer(doc.activeLayer()));
}

}  // namespace

PE_TEST(layerspanel_shows_a_thumbnail_on_a_document_larger_than_the_old_cap) {
    // The defect: the panel refused outright above 4 MP, so every pixel layer's icon was null
    // on any document from a real camera, while the MASK column kept working. Behind that sat
    // a second silent cliff at the composite cap.
    auto doc = pe::Document::createBlank(pe::Size{3000, 3000});  // 9 MP, over the old cap
    PE_REQUIRE(doc != nullptr);
    baseLayer(*doc)->tiles().fillRect(pe::Rect{0, 0, 1500, 1500}, pe::Rgba8{0, 128, 255, 255});

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);
    PE_REQUIRE(tree->topLevelItemCount() == 1);

    const QImage img = iconImage(tree->topLevelItem(0), 0);
    PE_CHECK(!img.isNull());
    // The filled top-left quadrant really is that colour, not merely "an icon exists".
    PE_CHECK(nearColor(img.pixelColor(5, 5), QColor(0, 128, 255)));
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_thumbnail_keeps_the_content_where_it_sits_on_the_canvas) {
    // The framing decision, pinned. The downscale comes from the CANVAS, so a mark appears
    // where it lies rather than being stretched to fill the icon. A naive implementation that
    // scaled contentBounds up to 26x26 would render a single dab as a full-thumbnail blob and
    // lose the one thing the preview is for: telling you what is on this layer and where.
    auto doc = pe::Document::createBlank(pe::Size{2000, 2000});
    PE_REQUIRE(doc != nullptr);
    baseLayer(*doc)->tiles().fillRect(pe::Rect{0, 0, 500, 500}, pe::Rgba8{255, 0, 0, 255});

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);
    const QImage img = iconImage(tree->topLevelItem(0), 0);
    PE_REQUIRE(!img.isNull());

    PE_CHECK(nearColor(img.pixelColor(3, 3), QColor(255, 0, 0)));  // the quarter that is filled
    PE_CHECK(isChecker(img.pixelColor(20, 20)));                   // and the three that are not
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_an_empty_layer_shows_the_checker_and_not_black) {
    // A transparent layer must read as transparent. Premultiplied averaging over an
    // all-transparent region divides by a zero alpha sum, and getting that wrong yields
    // opaque black, which looks like a layer full of content.
    auto doc = pe::Document::createBlank(pe::Size{2000, 2000});
    PE_REQUIRE(doc != nullptr);

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);
    const QImage img = iconImage(tree->topLevelItem(0), 0);
    PE_REQUIRE(!img.isNull());

    int nonChecker = 0;
    for (int y = 2; y < kThumb - 2; ++y) {
        for (int x = 2; x < kThumb - 2; ++x) {
            if (!isChecker(img.pixelColor(x, y))) ++nonChecker;
        }
    }
    PE_CHECK_EQ(nonChecker, 0);
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_a_property_change_refreshes_one_row_not_the_whole_tree) {
    // rebuild() calls QTreeWidget::clear(), which destroys and recreates every row, so
    // pointer identity is an exact proxy for "the tree was rebuilt". It also recomposites
    // every layer's thumbnail, so toggling one layer's visibility on a twenty-layer document
    // did twenty composites and discarded the selection and expansion state on the way.
    auto doc = pe::Document::createBlank(pe::Size{256, 256});
    PE_REQUIRE(doc != nullptr);
    for (int i = 0; i < 4; ++i) {
        auto extra = std::make_unique<pe::PixelLayer>("L" + std::to_string(i));
        extra->tiles().setPixel(i * 10, 5, pe::Rgba8{200, 50, 50, 255});
        doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(extra));
    }

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);
    PE_REQUIRE(tree->topLevelItemCount() == 5);

    QTreeWidgetItem* const rows[5] = {tree->topLevelItem(0), tree->topLevelItem(1),
                                      tree->topLevelItem(2), tree->topLevelItem(3),
                                      tree->topLevelItem(4)};
    // Hide one layer: a LayerProps change naming that layer.
    const pe::LayerId target = doc->topLevelLayers()[2]->id();
    doc->history().push(std::make_unique<pe::SetVisibilityCommand>(target, false));

    for (int i = 0; i < 5; ++i) {
        PE_CHECK(tree->topLevelItem(i) == rows[i]);  // same objects: not rebuilt
    }
    // And the row really did update rather than being left stale.
    QTreeWidgetItem* changed = nullptr;
    for (int i = 0; i < 5; ++i) {
        if (tree->topLevelItem(i)->text(0) == QStringLiteral("L1")) changed = tree->topLevelItem(i);
    }
    PE_REQUIRE(changed != nullptr);
    PE_CHECK(changed->checkState(0) == Qt::Unchecked);
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_thumbnail_cost_follows_the_content_not_the_canvas) {
    // The whole point of the change, and invisible in the pixels: the output is 26x26 either
    // way, so a preview that reads the entire canvas and one that reads only the layer's own
    // content produce the same icon. Only the tile count tells them apart.
    //
    // Two documents of very different sizes, each with one small dab. The work must be the
    // same, and it must be a handful of tiles rather than the canvas.
    std::uint64_t small = 0;
    std::uint64_t large = 0;
    for (const int side : {512, 8192}) {
        auto doc = pe::Document::createBlank(pe::Size{side, side});
        PE_REQUIRE(doc != nullptr);
        baseLayer(*doc)->tiles().fillRect(pe::Rect{0, 0, 40, 40}, pe::Rgba8{10, 200, 90, 255});

        const std::uint64_t before = pe::scaledCompositeTileCount();
        pe::app::LayersPanel panel;
        panel.setDocument(doc.get());
        const std::uint64_t used = pe::scaledCompositeTileCount() - before;
        (side == 512 ? small : large) = used;

        QTreeWidget* tree = treeOf(panel);
        PE_REQUIRE(tree != nullptr);
        PE_CHECK(!iconImage(tree->topLevelItem(0), 0).isNull());  // and it still drew something
        panel.setDocument(nullptr);
    }
    // A canvas 256 times the area costs the same, because the content is the same.
    PE_CHECK_EQ(small, large);
    PE_CHECK(small > 0);
    PE_CHECK(small <= 4);  // the dab's tile, plus at most its neighbours from the snap
}

PE_TEST(layerspanel_group_and_adjustment_rows_keep_their_glyphs) {
    // Neither is composited, and for an adjustment layer that is a safety property as well as
    // a cosmetic one: AdjustmentLayer::contentBounds() is the whole representable plane, so a
    // bounded composite handed that region would be asked for an astronomical number of
    // tiles. The glyph short-circuit is what keeps it away from that.
    auto doc = pe::Document::createBlank(pe::Size{2000, 2000});
    PE_REQUIRE(doc != nullptr);
    baseLayer(*doc)->tiles().fillRect(pe::Rect{0, 0, 2000, 2000}, pe::Rgba8{255, 0, 0, 255});

    auto group = std::make_unique<pe::GroupLayer>("G");
    group->addChild(std::make_unique<pe::PixelLayer>("inner"));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);

    bool sawGroup = false;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = tree->topLevelItem(i);
        if (item->text(0) != QStringLiteral("G")) continue;
        sawGroup = true;
        const QImage img = iconImage(item, 0);
        PE_CHECK(!img.isNull());
        // The folder glyph, not the red fill of the layer beneath it.
        int red = 0;
        for (int y = 0; y < kThumb; ++y) {
            for (int x = 0; x < kThumb; ++x) {
                if (nearColor(img.pixelColor(x, y), QColor(255, 0, 0))) ++red;
            }
        }
        PE_CHECK_EQ(red, 0);
    }
    PE_CHECK(sawGroup);
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_a_layer_can_be_renamed_in_place_and_undone) {
    // Layer names were visible, meaningful and FIXED at creation: rows carried
    // ItemIsUserCheckable but never ItemIsEditable, and no rename existed anywhere in the
    // application. Double-clicking a plain layer did nothing at all.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    const pe::LayerId id = doc->activeLayer();
    const std::string original = doc->findLayer(id)->name();

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);
    PE_REQUIRE(tree->topLevelItemCount() == 1);

    // The flag the USER needs, asserted separately: setText below commits an edit whether or
    // not the row is editable, so without this the double-click path is unpinned.
    PE_CHECK((tree->topLevelItem(0)->flags() & Qt::ItemIsEditable) != 0);

    // Editing the row's text is what committing an in-place edit does.
    tree->topLevelItem(0)->setText(0, QStringLiteral("Sky"));
    PE_CHECK(doc->findLayer(id)->name() == std::string("Sky"));
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));  // one command

    doc->history().undo();
    PE_CHECK(doc->findLayer(id)->name() == original);
    // And the row followed the undo rather than keeping the typed text.
    PE_CHECK(tree->topLevelItem(0)->text(0) == QString::fromStdString(original));
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_an_empty_rename_is_refused_rather_than_committed) {
    // An empty name leaves a row nothing can identify, and it would be a real command on the
    // undo stack. The old text goes back and nothing is pushed.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    const pe::LayerId id = doc->activeLayer();
    const std::string original = doc->findLayer(id)->name();

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);

    tree->topLevelItem(0)->setText(0, QStringLiteral("   "));  // whitespace only
    PE_CHECK(doc->findLayer(id)->name() == original);
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(0));
    PE_CHECK(tree->topLevelItem(0)->text(0) == QString::fromStdString(original));

    // And renaming to the SAME name pushes nothing either: one signal carries both the text
    // and the check state, so an edit that changed neither must be a no-op.
    tree->topLevelItem(0)->setText(0, QString::fromStdString(original));
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(0));
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_toggling_visibility_still_works_alongside_rename) {
    // The inverse of the case above: itemChanged now handles two different edits, so the
    // check-state path must not have been swallowed by the rename branch.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    const pe::LayerId id = doc->activeLayer();

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);

    PE_CHECK(doc->findLayer(id)->visible());
    tree->topLevelItem(0)->setCheckState(0, Qt::Unchecked);
    PE_CHECK(!doc->findLayer(id)->visible());
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_deleting_a_layer_with_content_asks_first) {
    // Delete removed the layer immediately, whatever was on it. The prompt is injected rather
    // than owned by the panel, so the rule is testable and the modal lives in one place.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    auto second = std::make_unique<pe::PixelLayer>("Painted");
    second->tiles().fillRect(pe::Rect{0, 0, 20, 20}, pe::Rgba8{200, 50, 50, 255});
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(second));
    const pe::LayerId painted = doc->topLevelLayers()[1]->id();
    doc->setActiveLayer(painted);

    pe::app::LayersPanel panel;
    int asked = 0;
    QString askedAbout;
    panel.setDeleteConfirmer([&asked, &askedAbout](const QString& name) {
        ++asked;
        askedAbout = name;
        return false;  // the user says no
    });
    panel.setDocument(doc.get());

    QPushButton* del = panel.findChild<QPushButton*>();
    PE_REQUIRE(del != nullptr);
    // Reach the Delete button by its accessible name rather than its position.
    QPushButton* deleteBtn = nullptr;
    for (QPushButton* b : panel.findChildren<QPushButton*>()) {
        if (b->accessibleName() == QStringLiteral("Delete layer")) deleteBtn = b;
    }
    PE_REQUIRE(deleteBtn != nullptr);

    deleteBtn->click();
    PE_CHECK_EQ(asked, 1);
    PE_CHECK(askedAbout == QStringLiteral("Painted"));
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));  // refused: still there

    panel.setDeleteConfirmer([&asked](const QString&) {
        ++asked;
        return true;  // and now the user says yes
    });
    deleteBtn->click();
    PE_CHECK_EQ(asked, 2);
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_deleting_an_empty_layer_does_not_ask) {
    // Prompting for a layer that holds nothing trains the user to dismiss the dialog unread,
    // which is how a confirmation stops confirming anything.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::make_unique<pe::PixelLayer>("Empty"));
    doc->setActiveLayer(doc->topLevelLayers()[1]->id());

    pe::app::LayersPanel panel;
    int asked = 0;
    panel.setDeleteConfirmer([&asked](const QString&) {
        ++asked;
        return true;
    });
    panel.setDocument(doc.get());

    QPushButton* deleteBtn = nullptr;
    for (QPushButton* b : panel.findChildren<QPushButton*>()) {
        if (b->accessibleName() == QStringLiteral("Delete layer")) deleteBtn = b;
    }
    PE_REQUIRE(deleteBtn != nullptr);
    deleteBtn->click();
    PE_CHECK_EQ(asked, 0);
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(1));  // and it went
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_every_button_has_a_tooltip_and_an_accessible_name) {
    // Three of eight had a tooltip and none had an accessible name. Two of them are a bare
    // Unicode triangle, which assistive technology announces as a shape or skips entirely,
    // and abbreviations like "Dup" and "Msk" are not much better for a sighted first-time
    // user either.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());

    const QList<QPushButton*> buttons = panel.findChildren<QPushButton*>();
    PE_CHECK_EQ(buttons.size(), 8);
    for (QPushButton* b : buttons) {
        PE_CHECK(!b->toolTip().isEmpty());
        PE_CHECK(!b->accessibleName().isEmpty());
        // The name must say what it does, not repeat a glyph.
        PE_CHECK(b->accessibleName() != b->text());
    }
    panel.setDocument(nullptr);
}
