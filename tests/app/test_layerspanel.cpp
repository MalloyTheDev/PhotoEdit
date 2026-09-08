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
#include "pe/core/Mask.hpp"
#include "pe/core/PixelLayer.hpp"
#include "pe/core/Refusal.hpp"
#include "pe_test.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QAbstractItemView>
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QString>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QWidget>

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

// --- drag reorder (#131) -------------------------------------------------------------
//
// The panel advertised drag and drop by being a tree and then did nothing when you dragged
// a row: no move, no message. What follows tests the rule, not Qt's mouse handling. The one
// step deliberately not covered is pixel position -> DropPlace, which is Qt's own
// dragMoveEvent: under InternalMove it discards any event whose source() is not the view,
// so a synthesized drag never sets the drop indicator and there is nothing to assert on.
// Everything after that point belongs to the shell, and handleLayerDrop is where it starts.

namespace {

std::string describeRows(const std::vector<int>& rows) {
    std::string out;
    for (int v : rows) out += (out.empty() ? "" : ",") + std::to_string(v);
    return out;
}

// Apply reorderTargetForDrop and report the resulting ROW order (top row first), each row
// named by the engine index its layer started at.
std::vector<int> rowsAfterDrop(int n, int from, int insertBefore) {
    std::vector<int> engine;  // bottom-first, holding original indices
    for (int i = 0; i < n; ++i) engine.push_back(i);

    const std::size_t to =
        pe::app::reorderTargetForDrop(static_cast<std::size_t>(from), insertBefore, n);
    if (to != pe::GroupLayer::npos) {
        const int v = engine[static_cast<std::size_t>(from)];
        engine.erase(engine.begin() + from);
        // Clamped exactly as GroupLayer::insertChild clamps, so an over-large target shows
        // up here as the wrong ARRANGEMENT (what the user would see) rather than as
        // undefined behaviour in the harness.
        const std::size_t at = std::min(to, engine.size());
        engine.insert(engine.begin() + static_cast<std::ptrdiff_t>(at), v);
    }
    return std::vector<int>(engine.rbegin(), engine.rend());  // rows read top-first
}

// What the drop ASKED for, worked out independently and entirely in row space: pull the
// dragged row out of the list, then put it back at the requested slot.
std::vector<int> rowsRequested(int n, int from, int insertBefore) {
    std::vector<int> rows;
    for (int i = n - 1; i >= 0; --i) rows.push_back(i);  // top-first
    const auto it = std::find(rows.begin(), rows.end(), from);
    const int fromRow = static_cast<int>(std::distance(rows.begin(), it));
    rows.erase(it);
    int at = insertBefore;
    if (fromRow < at) --at;  // the removal closed a slot above the target
    rows.insert(rows.begin() + at, from);
    return rows;
}

}  // namespace

PE_TEST(layerspanel_a_drop_lands_the_layer_where_the_indicator_said) {
    // Rows count down from the top while the engine counts up from the bottom, and the
    // command removes the layer before reinserting it, so there are two separate chances to
    // land one slot off. Checked exhaustively against an oracle that never leaves row space.
    int bad = 0;
    for (int n = 1; n <= 6; ++n) {
        for (int from = 0; from < n; ++from) {
            for (int insertBefore = 0; insertBefore <= n; ++insertBefore) {
                const std::vector<int> got = rowsAfterDrop(n, from, insertBefore);
                const std::vector<int> want = rowsRequested(n, from, insertBefore);
                if (got == want) continue;
                if (++bad <= 4) {  // enough to see the pattern, not a wall of output
                    std::printf("    drop n=%d from=%d before=%d gave [%s], wanted [%s]\n", n, from,
                                insertBefore, describeRows(got).c_str(),
                                describeRows(want).c_str());
                }
            }
        }
    }
    PE_CHECK_EQ(bad, 0);

    // A drop asking for the position the layer already holds is not a reorder: pushing one
    // would leave a history entry that undoes to the same picture.
    PE_CHECK(pe::app::reorderTargetForDrop(2, 1, 4) == pe::GroupLayer::npos);  // above its row
    PE_CHECK(pe::app::reorderTargetForDrop(2, 2, 4) == pe::GroupLayer::npos);  // below its row
    // Nor is a drop against a stack that cannot hold the dragged layer.
    PE_CHECK(pe::app::reorderTargetForDrop(0, 0, 0) == pe::GroupLayer::npos);
    PE_CHECK(pe::app::reorderTargetForDrop(7, 0, 4) == pe::GroupLayer::npos);
}

PE_TEST(layerspanel_dragging_a_row_reorders_the_document_and_undoes) {
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::make_unique<pe::PixelLayer>("Middle"));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::make_unique<pe::PixelLayer>("Top"));

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);
    PE_REQUIRE(tree->topLevelItemCount() == 3);
    // Rows read top-first, so row 0 holds the layer at the highest engine index.
    PE_REQUIRE(tree->topLevelItem(0)->text(0) == QStringLiteral("Top"));

    // Drag the top row down and drop it below the bottom row: it becomes the bottom layer.
    panel.handleLayerDrop(tree->topLevelItem(0), tree->topLevelItem(2), pe::app::DropPlace::Below);
    PE_CHECK(doc->topLevelLayers()[0]->name() == std::string("Top"));
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));

    doc->history().undo();
    PE_CHECK(doc->topLevelLayers()[2]->name() == std::string("Top"));
    // The rows followed the undo rather than keeping the dragged arrangement.
    PE_REQUIRE(tree->topLevelItemCount() == 3);
    PE_CHECK(tree->topLevelItem(0)->text(0) == QStringLiteral("Top"));

    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(0));  // the undo took it

    // Dropping a row into the gap it already occupies is not an edit, on either side: a
    // command here would leave a history entry that undoes to the same picture.
    panel.handleLayerDrop(tree->topLevelItem(0), tree->topLevelItem(0), pe::app::DropPlace::Above);
    panel.handleLayerDrop(tree->topLevelItem(0), tree->topLevelItem(0), pe::app::DropPlace::Below);
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(0));
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_a_drop_past_the_last_row_sends_the_layer_to_the_bottom) {
    // The empty space below the rows is a real drop target, and the one that reads as "put
    // this at the very bottom". Qt reports it with a null index, so there is no target row.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::make_unique<pe::PixelLayer>("Top"));

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);
    PE_REQUIRE(tree->topLevelItemCount() == 2);
    PE_REQUIRE(tree->topLevelItem(0)->text(0) == QStringLiteral("Top"));

    panel.handleLayerDrop(tree->topLevelItem(0), nullptr, pe::app::DropPlace::PastEnd);
    PE_CHECK(doc->topLevelLayers()[0]->name() == std::string("Top"));
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(1));
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_a_drop_that_would_change_nesting_says_so) {
    // ReorderLayerCommand takes a TOP-LEVEL index, so dragging into or out of a group is not
    // something the engine can do yet. The panel has to say that, rather than put the layer
    // somewhere else instead, and rather than swallow the drag the way it used to.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    auto group = std::make_unique<pe::GroupLayer>("G");
    group->addChild(std::make_unique<pe::PixelLayer>("inner"));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));

    pe::app::LayersPanel panel;
    std::vector<pe::Refusal> said;
    QObject::connect(&panel, &pe::app::LayersPanel::refused,
                     [&said](const pe::Refusal& r) { said.push_back(r); });
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);
    PE_REQUIRE(tree->topLevelItemCount() == 2);

    QTreeWidgetItem* groupRow = tree->topLevelItem(0);  // inserted last, so it is the top row
    PE_REQUIRE(groupRow->text(0) == QStringLiteral("G"));
    PE_REQUIRE(groupRow->childCount() == 1);
    QTreeWidgetItem* inner = groupRow->child(0);
    QTreeWidgetItem* other = tree->topLevelItem(1);

    // Dropping ON a row means "inside it" in a tree.
    panel.handleLayerDrop(other, groupRow, pe::app::DropPlace::OnRow);
    PE_REQUIRE(said.size() == 1);
    PE_CHECK(said[0].code == pe::RefusalCode::Unsupported);
    PE_CHECK(!said[0].explanation.empty());

    // Dropping between a group's children is the same request by another route.
    panel.handleLayerDrop(other, inner, pe::app::DropPlace::Above);
    PE_REQUIRE(said.size() == 2);
    PE_CHECK(said[1].code == pe::RefusalCode::Unsupported);

    // And dragging a nested layer out of its group is refused with the reason that names the
    // way out: ungroup it first.
    panel.handleLayerDrop(inner, other, pe::app::DropPlace::Below);
    PE_REQUIRE(said.size() == 3);
    PE_CHECK(said[2].code == pe::RefusalCode::LayerNotTopLevel);

    // None of the three touched the document.
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(0));
    PE_CHECK_EQ(doc->topLevelCount(), static_cast<std::size_t>(2));
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_the_tree_is_actually_wired_for_dragging) {
    // The rule above is unreachable if the view never starts a drag, which is exactly the
    // state the panel shipped in: a QTreeWidget left at its default NoDragDrop.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr);

    PE_CHECK(tree->dragDropMode() == QAbstractItemView::InternalMove);
    PE_CHECK(tree->dragEnabled());
    PE_CHECK(tree->viewport()->acceptDrops());
    // And the rows themselves have to be draggable, or the view has nothing to pick up.
    PE_REQUIRE(tree->topLevelItemCount() >= 1);
    PE_CHECK((tree->topLevelItem(0)->flags() & Qt::ItemIsDragEnabled) != 0);
    panel.setDocument(nullptr);
}

// --- arrange ---------------------------------------------------------------------------

PE_TEST(layerspanel_arrange_moves_the_active_layer_through_the_stack) {
    // The two arrows computed their target inline, in opposite directions, and there was no
    // way to reach either end of the stack in one step. One rule now, stated once.
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::make_unique<pe::PixelLayer>("Middle"));
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::make_unique<pe::PixelLayer>("Top"));
    const pe::LayerId bottom = doc->topLevelLayers()[0]->id();
    doc->setActiveLayer(bottom);

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());

    // Engine index 0 is the BOTTOM of the stack, so "front" is the highest index.
    panel.arrangeActive(pe::app::LayersPanel::Arrange::Forward);
    PE_CHECK_EQ(doc->topLevelIndexOf(bottom), static_cast<std::size_t>(1));
    panel.arrangeActive(pe::app::LayersPanel::Arrange::Front);
    PE_CHECK_EQ(doc->topLevelIndexOf(bottom), static_cast<std::size_t>(2));
    panel.arrangeActive(pe::app::LayersPanel::Arrange::Backward);
    PE_CHECK_EQ(doc->topLevelIndexOf(bottom), static_cast<std::size_t>(1));
    panel.arrangeActive(pe::app::LayersPanel::Arrange::Back);
    PE_CHECK_EQ(doc->topLevelIndexOf(bottom), static_cast<std::size_t>(0));

    // Four moves, four undo steps, and the whole trip reverses.
    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(4));
    for (int i = 0; i < 4; ++i) doc->history().undo();
    PE_CHECK_EQ(doc->topLevelIndexOf(bottom), static_cast<std::size_t>(0));
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_arrange_that_cannot_move_the_layer_says_so) {
    // Both arrows were silent at the ends of the stack: the button stayed enabled, the
    // click did nothing, and nothing said why. That is the exact shape of "dead UI".
    auto doc = pe::Document::createBlank(pe::Size{64, 64});
    PE_REQUIRE(doc != nullptr);
    auto group = std::make_unique<pe::GroupLayer>("G");
    group->addChild(std::make_unique<pe::PixelLayer>("inner"));
    const pe::LayerId inner = group->children()[0]->id();
    doc->cmdInsertTopLevel(doc->topLevelCount(), std::move(group));

    pe::app::LayersPanel panel;
    std::vector<pe::Refusal> said;
    QObject::connect(&panel, &pe::app::LayersPanel::refused,
                     [&said](const pe::Refusal& r) { said.push_back(r); });
    panel.setDocument(doc.get());

    // The top layer cannot go further forward, in either flavour.
    doc->setActiveLayer(doc->topLevelLayers()[1]->id());
    panel.arrangeActive(pe::app::LayersPanel::Arrange::Forward);
    panel.arrangeActive(pe::app::LayersPanel::Arrange::Front);
    PE_REQUIRE(said.size() == 2);
    PE_CHECK(said[0].code == pe::RefusalCode::NoEffect);
    PE_CHECK(said[1].code == pe::RefusalCode::NoEffect);
    PE_CHECK(said[0].explanation.find("top") != std::string::npos);

    // Nor the bottom layer further back.
    doc->setActiveLayer(doc->topLevelLayers()[0]->id());
    panel.arrangeActive(pe::app::LayersPanel::Arrange::Backward);
    PE_REQUIRE(said.size() == 3);
    PE_CHECK(said[2].code == pe::RefusalCode::NoEffect);
    PE_CHECK(said[2].explanation.find("bottom") != std::string::npos);

    // And a nested layer has no top-level index to move, which is a different reason and
    // gets a different one.
    doc->setActiveLayer(inner);
    panel.arrangeActive(pe::app::LayersPanel::Arrange::Front);
    PE_REQUIRE(said.size() == 4);
    PE_CHECK(said[3].code == pe::RefusalCode::LayerNotTopLevel);

    PE_CHECK_EQ(doc->history().undoDepth(), static_cast<std::size_t>(0));  // nothing pushed
    panel.setDocument(nullptr);
}

// --- thumbnail framing -------------------------------------------------------------------

PE_TEST(layerspanel_a_thumbnail_frames_the_document_and_centres_it) {
    // The checkerboard filled the whole 26x26 icon and the document was pinned to the top
    // left, so a landscape canvas (which is most of them) left a band of checker along the
    // bottom of every thumbnail. Checker means "transparent", so that band read as empty
    // canvas when it is not part of the document at all.
    auto doc = pe::Document::createBlank(pe::Size{1200, 800});  // 3:2, so 26 x 17 in the icon
    PE_REQUIRE(doc != nullptr);
    baseLayer(*doc)->tiles().fillRect(pe::Rect{0, 0, 1200, 800}, pe::Rgba8{220, 40, 40, 255});

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr && tree->topLevelItemCount() == 1);
    const QImage img = iconImage(tree->topLevelItem(0), 0);
    PE_REQUIRE(!img.isNull());
    PE_REQUIRE(img.width() == kThumb && img.height() == kThumb);

    // A 3:2 document in a square icon leaves four transparent rows top and bottom, and none
    // at the sides. Counting the opaque band is how the centring is asserted without
    // hard-coding the divisor arithmetic.
    int firstOpaqueRow = -1;
    int lastOpaqueRow = -1;
    for (int y = 0; y < kThumb; ++y) {
        bool any = false;
        for (int x = 0; x < kThumb && !any; ++x) any = img.pixelColor(x, y).alpha() > 0;
        if (!any) continue;
        if (firstOpaqueRow < 0) firstOpaqueRow = y;
        lastOpaqueRow = y;
    }
    PE_REQUIRE(firstOpaqueRow >= 0);
    // Centred: the same number of blank rows above as below, to within the odd pixel.
    const int above = firstOpaqueRow;
    const int below = kThumb - 1 - lastOpaqueRow;
    PE_CHECK(above > 0);  // a landscape document does not reach the top of the icon
    PE_CHECK(std::abs(above - below) <= 1);
    // And the document is not squashed: it spans the full width.
    PE_CHECK(img.pixelColor(0, kThumb / 2).alpha() > 0);
    PE_CHECK(img.pixelColor(kThumb - 1, kThumb / 2).alpha() > 0);

    // The band itself is the layer's colour, not checker.
    PE_CHECK(nearColor(img.pixelColor(kThumb / 2, kThumb / 2), QColor(220, 40, 40)));
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_a_square_document_fills_the_thumbnail) {
    // The inverse: nothing is trimmed when the aspect already matches, so the centring did
    // not just shrink every thumbnail.
    auto doc = pe::Document::createBlank(pe::Size{800, 800});
    PE_REQUIRE(doc != nullptr);
    baseLayer(*doc)->tiles().fillRect(pe::Rect{0, 0, 800, 800}, pe::Rgba8{40, 200, 40, 255});

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr && tree->topLevelItemCount() == 1);
    const QImage img = iconImage(tree->topLevelItem(0), 0);
    PE_REQUIRE(!img.isNull());

    PE_CHECK(img.pixelColor(0, 0).alpha() > 0);
    PE_CHECK(img.pixelColor(kThumb - 1, kThumb - 1).alpha() > 0);
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_a_selected_rows_thumbnail_shows_its_own_pixels) {
    // A QIcon carrying only a Normal pixmap makes Qt synthesize the Selected one by blending
    // it toward the highlight colour. The active layer is the row the user is working on, so
    // it was the one row whose thumbnail did not show the layer's actual colours.
    auto doc = pe::Document::createBlank(pe::Size{400, 400});
    PE_REQUIRE(doc != nullptr);
    baseLayer(*doc)->tiles().fillRect(pe::Rect{0, 0, 400, 400}, pe::Rgba8{20, 120, 60, 255});

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr && tree->topLevelItemCount() == 1);
    const QIcon icon = tree->topLevelItem(0)->icon(0);

    const QImage normal = icon.pixmap(QSize(kThumb, kThumb), QIcon::Normal).toImage();
    const QImage selected = icon.pixmap(QSize(kThumb, kThumb), QIcon::Selected).toImage();
    const QImage active = icon.pixmap(QSize(kThumb, kThumb), QIcon::Active).toImage();
    PE_REQUIRE(!normal.isNull() && !selected.isNull());
    PE_CHECK(selected == normal);
    PE_CHECK(active == normal);
    // And it really is the layer's colour, so the comparison is not two blank images.
    PE_CHECK(nearColor(normal.pixelColor(kThumb / 2, kThumb / 2), QColor(20, 120, 60)));
    panel.setDocument(nullptr);
}

PE_TEST(layerspanel_a_mask_thumbnail_is_framed_like_the_layer_beside_it) {
    // The mask was sampled onto the full 26x26 square while the layer thumbnail next to it
    // was aspect-fitted, so on any non-square canvas the two disagreed about where a
    // masked-out region sat.
    auto doc = pe::Document::createBlank(pe::Size{1200, 800});
    PE_REQUIRE(doc != nullptr);
    baseLayer(*doc)->tiles().fillRect(pe::Rect{0, 0, 1200, 800}, pe::Rgba8{200, 200, 200, 255});
    auto mask = std::make_unique<pe::Mask>();
    mask->buffer().fillRect(pe::Rect{0, 0, 1200, 800}, 255);
    doc->findLayer(doc->activeLayer())->setMask(std::move(mask));

    pe::app::LayersPanel panel;
    panel.setDocument(doc.get());
    QTreeWidget* tree = treeOf(panel);
    PE_REQUIRE(tree != nullptr && tree->topLevelItemCount() == 1);
    const QImage layerImg = iconImage(tree->topLevelItem(0), 0);
    const QImage maskImg = iconImage(tree->topLevelItem(0), 1);
    PE_REQUIRE(!layerImg.isNull() && !maskImg.isNull());

    // Same frame: the rows that are outside the document in one are outside it in the other.
    int mismatched = 0;
    for (int y = 0; y < kThumb; ++y) {
        for (int x = 0; x < kThumb; ++x) {
            const bool inLayer = layerImg.pixelColor(x, y).alpha() > 0;
            const bool inMask = maskImg.pixelColor(x, y).alpha() > 0;
            if (inLayer != inMask) ++mismatched;
        }
    }
    PE_CHECK_EQ(mismatched, 0);
    // And it is not vacuously true because both are empty.
    PE_CHECK(maskImg.pixelColor(kThumb / 2, kThumb / 2).alpha() > 0);
    PE_CHECK(maskImg.pixelColor(kThumb / 2, 0).alpha() == 0);  // trimmed top, like the layer
    panel.setDocument(nullptr);
}
