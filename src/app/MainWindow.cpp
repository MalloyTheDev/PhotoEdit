#include "MainWindow.hpp"

#include "PixelFormatNames.hpp"

#include "AdjustmentsPanel.hpp"
#include "BusyTask.hpp"
#include "CanvasView.hpp"
#include "ChannelsPanel.hpp"
#include "ColorPanel.hpp"
#include "CurvesDialog.hpp"
#include "EffectDialog.hpp"
#include "ExportDialog.hpp"
#include "GradientsPanel.hpp"
#include "GroupedSlidersDialog.hpp"
#include "HistoryPanel.hpp"
#include "IconUtil.hpp"
#include "LayersPanel.hpp"
#include "PropertiesPanel.hpp"
#include "SwatchesPanel.hpp"
#include "TextDialog.hpp"
#include "TextRender.hpp"
#include "pe/core/Adjustment.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Brush.hpp"  // pe::PaintCommand (effect-dialog command factories)
#include "pe/core/Color.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Compositor.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/ImageIO.hpp"
#include "pe/core/TextLayer.hpp"  // pe::TextLayer / TextModel / EditTextCommand
#include "pe/core/Version.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointF>
#include <QSettings>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pe::app {

namespace {
// File-dialog filter covering the formats the engine can read/write.
constexpr const char* kOpenFilter =
    "Images (*.pedoc *.png *.jpg *.jpeg *.tif *.tiff *.webp *.psd);;All files (*)";
constexpr const char* kSaveFilter =
    "PhotoEdit document (*.pedoc);;PNG (*.png);;JPEG (*.jpg);;TIFF (*.tif);;WebP (*.webp)";

// A calm light tint for resting tool icons (the active one is marked by an accent
// outline, so the glyph itself stays restrained).

// One tool-strip entry. `tool` == Inactive marks a scaffolded, not-yet-wired tool.
struct ToolDef {
    const char* icon;
    const char* label;
    CanvasView::Tool tool;
    const char* shortcut;
    // One line naming the gesture, shown in the status bar beside the tool name.
    // Empty for scaffolded tools, whose label already says they do nothing yet.
    const char* hint;
};
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    resize(1280, 800);
    fgColor_ = QColor(0, 0, 0);        // Photoshop defaults: black foreground,
    bgColor_ = QColor(255, 255, 255);  // white background

    canvas_ = new CanvasView(this);
    // The canvas's own long tools take the window's re-entrancy guard, exactly as the File
    // actions do. Without this the Magic Wand ran a second worker inside a running save.
    canvas_->setTaskRunner(
        [this](const QString& title, TaskAccess access, const std::function<void()>& work) {
            return runGuardedTask(title, access, work);
        });
    connect(canvas_, &CanvasView::colorPicked, this, &MainWindow::onColorPicked);
    connect(canvas_, &CanvasView::toolMessage, this,
            [this](const QString& msg) { statusBar()->showMessage(msg, 4000); });
    // Sibling, not nested inside the handler above, where it used to sit: a connect made
    // inside a slot runs every time that slot runs, so canvas refusals were dropped entirely
    // until the first tool message and then reported once per message after it.
    connect(canvas_, &CanvasView::refused, this, &MainWindow::reportRefusal);
    connect(canvas_, &CanvasView::textRequested, this, &MainWindow::onAddText);
    // The canvas can select a tool itself: grabbing a transform handle under the Move tool
    // enters Free Transform. The strip has to follow, or it keeps claiming a tool that is
    // not active. Triggering the action reuses the one path that also updates the status
    // bar and the options bar, instead of a second copy of it here.
    connect(canvas_, &CanvasView::toolChanged, this, [this](CanvasView::Tool t) {
        const auto it = toolActions_.constFind(static_cast<int>(t));
        if (it != toolActions_.constEnd() && *it != nullptr) {
            if (!(*it)->isChecked()) (*it)->trigger();
            return;
        }
        // Free Transform has no button in the strip: it is Edit > Free Transform (Ctrl+T).
        // So there is nothing to check, and the strip would otherwise keep the PREVIOUS
        // tool lit while the canvas transforms. That was already true of Ctrl+T before the
        // canvas could select a tool itself.
        if (t != CanvasView::Tool::Transform) return;
        if (toolGroup_ != nullptr) {
            if (QAction* lit = toolGroup_->checkedAction(); lit != nullptr) {
                lit->setChecked(false);
            }
        }
        if (toolLabel_ != nullptr) toolLabel_->setText(QStringLiteral("Free Transform"));
        if (toolHintLabel_ != nullptr) {
            toolHintLabel_->setText(QStringLiteral(
                "Drag a corner to scale, the knob to rotate. Enter applies, Esc cancels"));
        }
        updateOptionsBar(OptKind::None, QStringLiteral("Free Transform"));
    });

    buildMenuBar();
    buildToolBar();     // left tool strip (+ fg/bg swatches)
    buildOptionsBar();  // contextual tool options across the top
    buildCentral();     // document tab strip + canvas
    buildDockPanels();  // tabbed panel groups on the right
    buildStatusBar();
    populateWindowMenu();  // needs the docks, so it runs after buildDockPanels

    updateOptionsBar(OptKind::Brush, QStringLiteral("Brush"));  // Brush is the default tool
    refreshTitle();
    // The window opens with no document, and setDocument has not run, so the initial
    // disabled state has to be applied here or the very case this fixes stays broken.
    updateActionStates();
}

MainWindow::~MainWindow() {
    // Detach the observing widgets while doc_ is still alive: doc_ (a member) is
    // destroyed when this body returns, but the child widgets are deleted later by
    // the QObject base destructor, so clearing them now avoids a dangling
    // removeObserver() in their destructors.
    if (doc_ != nullptr) doc_->removeObserver(this);
    if (canvas_ != nullptr) canvas_->setDocument(nullptr);
    if (layers_ != nullptr) layers_->setDocument(nullptr);
    if (history_ != nullptr) history_->setDocument(nullptr);
    if (properties_ != nullptr) properties_->setDocument(nullptr);
    if (channels_ != nullptr) channels_->setDocument(nullptr);
}

namespace {

// QKeySequence's standard keys are per-platform tables, and some entries are empty
// on some platforms: Quit and Deselect resolve to nothing usable on Windows, and
// SaveAs resolves to nothing at all on Linux. Prefer the platform's own binding when
// it has one, and fall back to the conventional chord so an action is never left
// silently unbound on one platform.
[[nodiscard]] QKeySequence standardOr(QKeySequence::StandardKey key, const char* fallback) {
    const QKeySequence seq(key);
    return seq.isEmpty() ? QKeySequence(QString::fromUtf8(fallback)) : seq;
}

}  // namespace

void MainWindow::buildMenuBar() {
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileActions_.push_back(fileMenu->addAction(QStringLiteral("&New"),
                                               standardOr(QKeySequence::New, "Ctrl+N"), this,
                                               &MainWindow::newDocument));
    fileActions_.push_back(fileMenu->addAction(QStringLiteral("&Open..."),
                                               standardOr(QKeySequence::Open, "Ctrl+O"), this,
                                               &MainWindow::openDocument));
    fileMenu->addSeparator();
    docActions_.push_back(fileMenu->addAction(QStringLiteral("&Save"),
                                              standardOr(QKeySequence::Save, "Ctrl+S"), this,
                                              &MainWindow::saveDocument));
    docActions_.push_back(fileMenu->addAction(QStringLiteral("Save &As..."),
                                              standardOr(QKeySequence::SaveAs, "Ctrl+Shift+S"),
                                              this, &MainWindow::saveDocumentAs));
    // No StandardKey for export; Ctrl+Shift+E is the common convention.
    docActions_.push_back(fileMenu->addAction(QStringLiteral("E&xport As..."),
                                              QKeySequence(QStringLiteral("Ctrl+Shift+E")), this,
                                              &MainWindow::exportDocumentAs));
    fileActions_.insert(fileActions_.end(), docActions_.end() - 3, docActions_.end());
    fileMenu->addSeparator();
    // Explicit Ctrl+Q rather than QKeySequence::Quit: on Windows that standard key
    // resolves to the unusable literal "Exit" rather than a chord.
    // Routed through close() so closeEvent can guard unsaved work; connecting
    // QApplication::quit directly bypassed the guard entirely.
    fileActions_.push_back(fileMenu->addAction(
        QStringLiteral("E&xit"), QKeySequence(QStringLiteral("Ctrl+Q")), this, &MainWindow::close));

    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    undoAct_ = editMenu->addAction(QStringLiteral("&Undo"), this, &MainWindow::undo);
    undoAct_->setShortcut(QKeySequence::Undo);
    redoAct_ = editMenu->addAction(QStringLiteral("&Redo"), this, &MainWindow::redo);
    redoAct_->setShortcut(QKeySequence::Redo);
    editMenu->addSeparator();
    // Cut/Copy/Paste did not exist at all, in the shell or the engine, so the two most
    // conventional shortcuts in any editor did nothing. Ctrl+Shift+C is Copy Merged (the
    // composite rather than one layer) and Ctrl+Shift+V is Paste Into, both as in Photoshop.
    docActions_.push_back(editMenu->addAction(QStringLiteral("Cu&t"),
                                              standardOr(QKeySequence::Cut, "Ctrl+X"), this,
                                              [this] { cutToClipboard(); }));
    docActions_.push_back(editMenu->addAction(QStringLiteral("&Copy"),
                                              standardOr(QKeySequence::Copy, "Ctrl+C"), this,
                                              [this] { copyToClipboard(false); }));
    docActions_.push_back(editMenu->addAction(QStringLiteral("Copy &Merged"),
                                              QKeySequence(QStringLiteral("Ctrl+Shift+C")), this,
                                              [this] { copyToClipboard(true); }));
    docActions_.push_back(editMenu->addAction(QStringLiteral("&Paste"),
                                              standardOr(QKeySequence::Paste, "Ctrl+V"), this,
                                              [this] { pasteFromClipboard(false); }));
    docActions_.push_back(editMenu->addAction(QStringLiteral("Paste &Into"),
                                              QKeySequence(QStringLiteral("Ctrl+Shift+V")), this,
                                              [this] { pasteFromClipboard(true); }));
    docActions_.push_back(editMenu->addAction(QStringLiteral("Cl&ear"),
                                              standardOr(QKeySequence::Delete, "Del"), this,
                                              [this] { clearSelection(); }));
    editMenu->addSeparator();
    QAction* freeTransformAct =
        editMenu->addAction(QStringLiteral("Free &Transform"), this, [this] {
            if (canvas_ != nullptr) canvas_->setTool(CanvasView::Tool::Transform);
        });
    freeTransformAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));

    // Open a parameterized effect dialog (live preview + one-undo-step commit). Captured by
    // value into the action lambdas below, so it must hold only `this` (a stable pointer).
    auto runEffect = [this](const QString& title, std::vector<EffectDialog::Param> params,
                            EffectDialog::CommandFactory factory) {
        if (!doc_) return;
        EffectDialog dlg(this, title, std::move(params), std::move(factory), doc_.get(),
                         [this] { canvas_->reloadImage(); });
        connect(&dlg, &EffectDialog::refused, this, &MainWindow::reportRefusal);
        dlg.exec();
    };
    // Apply a no-parameter effect destructively and undoably to the active layer.
    auto applyInstant = [this](std::unique_ptr<pe::PaintCommand> cmd) {
        if (doc_ && cmd) doc_->history().push(std::move(cmd));
    };
    const auto sel = [this] { return &doc_->selection(); };  // active selection (gates edits)

    auto* imageMenu = menuBar()->addMenu(QStringLiteral("&Image"));
    docMenus_.push_back(imageMenu);
    {
        auto* adj = imageMenu->addMenu(QStringLiteral("Adjustments"));
        adj->addAction(QStringLiteral("Brightness/Contrast..."), this, [this, runEffect] {
            runEffect(
                QStringLiteral("Brightness/Contrast"),
                {{QStringLiteral("Brightness"), -1.0, 1.0, 0.0, 2},
                 {QStringLiteral("Contrast"), -1.0, 1.0, 0.0, 2}},
                [this](const std::vector<double>& v) {
                    return pe::applyAdjustment(
                        *doc_, doc_->activeLayer(),
                        pe::BrightnessContrast(static_cast<float>(v[0]), static_cast<float>(v[1])),
                        &doc_->selection());
                });
        });
        adj->addAction(QStringLiteral("Hue/Saturation..."), this, [this, runEffect] {
            runEffect(QStringLiteral("Hue/Saturation"),
                      {{QStringLiteral("Hue"), -180.0, 180.0, 0.0, 0},
                       {QStringLiteral("Saturation"), 0.0, 2.0, 1.0, 2},
                       {QStringLiteral("Lightness"), -1.0, 1.0, 0.0, 2}},
                      [this](const std::vector<double>& v) {
                          pe::HueSaturation hs;
                          hs.setHueShiftDegrees(static_cast<float>(v[0]));
                          hs.setSaturationScale(static_cast<float>(v[1]));
                          hs.setLightness(static_cast<float>(v[2]));
                          return pe::applyAdjustment(*doc_, doc_->activeLayer(), hs,
                                                     &doc_->selection());
                      });
        });
        adj->addAction(QStringLiteral("Exposure..."), this, [this, runEffect] {
            runEffect(QStringLiteral("Exposure"),
                      {{QStringLiteral("Exposure (stops)"), -5.0, 5.0, 0.0, 2},
                       {QStringLiteral("Offset"), -0.5, 0.5, 0.0, 3},
                       {QStringLiteral("Gamma"), 0.1, 5.0, 1.0, 2}},
                      [this](const std::vector<double>& v) {
                          return pe::applyAdjustment(
                              *doc_, doc_->activeLayer(),
                              pe::Exposure(static_cast<float>(v[0]), static_cast<float>(v[1]),
                                           static_cast<float>(v[2])),
                              &doc_->selection());
                      });
        });
        adj->addAction(QStringLiteral("Levels..."), this, [this, runEffect] {
            runEffect(QStringLiteral("Levels"),
                      {{QStringLiteral("Input Black"), 0.0, 1.0, 0.0, 2},
                       {QStringLiteral("Input White"), 0.0, 1.0, 1.0, 2},
                       {QStringLiteral("Gamma"), 0.1, 9.99, 1.0, 2},
                       {QStringLiteral("Output Black"), 0.0, 1.0, 0.0, 2},
                       {QStringLiteral("Output White"), 0.0, 1.0, 1.0, 2}},
                      [this](const std::vector<double>& v) {
                          pe::Levels lv;
                          lv.setInputBlack(static_cast<float>(v[0]));
                          lv.setInputWhite(static_cast<float>(v[1]));
                          lv.setGamma(static_cast<float>(v[2]));
                          lv.setOutputBlack(static_cast<float>(v[3]));
                          lv.setOutputWhite(static_cast<float>(v[4]));
                          return pe::applyAdjustment(*doc_, doc_->activeLayer(), lv,
                                                     &doc_->selection());
                      });
        });
        adj->addAction(QStringLiteral("Vibrance..."), this, [this, runEffect] {
            runEffect(QStringLiteral("Vibrance"),
                      {{QStringLiteral("Vibrance"), -1.0, 1.0, 0.0, 2},
                       {QStringLiteral("Saturation"), -1.0, 1.0, 0.0, 2}},
                      [this](const std::vector<double>& v) {
                          return pe::applyAdjustment(
                              *doc_, doc_->activeLayer(),
                              pe::Vibrance(static_cast<float>(v[0]), static_cast<float>(v[1])),
                              &doc_->selection());
                      });
        });
        adj->addAction(QStringLiteral("Color Balance..."), this, [this, runEffect] {
            runEffect(QStringLiteral("Color Balance"),
                      {{QStringLiteral("Cyan / Red"), -1.0, 1.0, 0.0, 2},
                       {QStringLiteral("Magenta / Green"), -1.0, 1.0, 0.0, 2},
                       {QStringLiteral("Yellow / Blue"), -1.0, 1.0, 0.0, 2}},
                      [this](const std::vector<double>& v) {
                          pe::ColorBalance cb;
                          cb.setMidtones(static_cast<float>(v[0]), static_cast<float>(v[1]),
                                         static_cast<float>(v[2]));
                          return pe::applyAdjustment(*doc_, doc_->activeLayer(), cb,
                                                     &doc_->selection());
                      });
        });
        adj->addAction(QStringLiteral("Posterize..."), this, [this, runEffect] {
            runEffect(QStringLiteral("Posterize"), {{QStringLiteral("Levels"), 2.0, 255.0, 4.0, 0}},
                      [this](const std::vector<double>& v) {
                          return pe::applyAdjustment(*doc_, doc_->activeLayer(),
                                                     pe::Posterize(static_cast<int>(v[0])),
                                                     &doc_->selection());
                      });
        });
        adj->addAction(QStringLiteral("Threshold..."), this, [this, runEffect] {
            runEffect(QStringLiteral("Threshold"), {{QStringLiteral("Level"), 0.0, 1.0, 0.5, 2}},
                      [this](const std::vector<double>& v) {
                          return pe::applyAdjustment(*doc_, doc_->activeLayer(),
                                                     pe::Threshold(static_cast<float>(v[0])),
                                                     &doc_->selection());
                      });
        });
        adj->addSeparator();
        adj->addAction(QStringLiteral("Invert"), this, [this, applyInstant, sel] {
            if (doc_)
                applyInstant(pe::applyAdjustment(*doc_, doc_->activeLayer(), pe::Invert{}, sel()));
        });
    }
    {
        auto* layerMenu = menuBar()->addMenu(QStringLiteral("&Layer"));
        docMenus_.push_back(layerMenu);
        // New / Duplicate / Delete and Arrange existed only as buttons in the Layers dock,
        // so a user who had closed that dock (or never found it) had no way to add a layer
        // at all, and no keyboard route to any of it. The panel owns the rules; the menu is
        // a second door onto them.
        QAction* newLayerAct = layerMenu->addAction(QStringLiteral("&New Layer"), this, [this] {
            if (layers_ != nullptr) layers_->addLayer();
        });
        newLayerAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+N")));
        QAction* dupLayerAct =
            layerMenu->addAction(QStringLiteral("&Duplicate Layer"), this, [this] {
                if (layers_ != nullptr) layers_->duplicateLayer();
            });
        dupLayerAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+J")));
        layerMenu->addAction(QStringLiteral("De&lete Layer"), this, [this] {
            if (layers_ != nullptr) layers_->deleteLayer();
        });

        clipAct_ = layerMenu->addAction(QStringLiteral("&Clip to Layer Below"), this, [this] {
            if (layers_ != nullptr) layers_->toggleClipToLayerBelow();
            // Qt flips a checkable action BEFORE the handler runs, so a refused toggle would
            // otherwise leave the menu claiming the layer is clipped when it is not. Nothing
            // else re-syncs it here: a refusal makes no document change to observe.
            updateActionStates();
        });
        clipAct_->setCheckable(true);
        clipAct_->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+G")));
        clipAct_->setToolTip(
            QStringLiteral("Confine this layer to the coverage of the layer beneath it"));

        layerMenu->addSeparator();
        auto* arrangeMenu = layerMenu->addMenu(QStringLiteral("&Arrange"));
        struct ArrangeEntry {
            const char* text;
            const char* keys;
            LayersPanel::Arrange where;
        };
        // Listed top of the stack first, so the menu reads the way the Layers dock does.
        static constexpr ArrangeEntry kArrangements[] = {
            {"Bring to &Front", "Ctrl+Shift+]", LayersPanel::Arrange::Front},
            {"Bring F&orward", "Ctrl+]", LayersPanel::Arrange::Forward},
            {"Send &Backward", "Ctrl+[", LayersPanel::Arrange::Backward},
            {"Send to Bac&k", "Ctrl+Shift+[", LayersPanel::Arrange::Back},
        };
        for (const ArrangeEntry& e : kArrangements) {
            QAction* act =
                arrangeMenu->addAction(QString::fromUtf8(e.text), this, [this, where = e.where] {
                    if (layers_ != nullptr) {
                        layers_->arrangeActive(where);
                    }
                });
            act->setShortcut(QKeySequence(QString::fromUtf8(e.keys)));
        }

        layerMenu->addSeparator();
        QAction* groupAct = layerMenu->addAction(QStringLiteral("&Group Layers"), this, [this] {
            if (layers_ != nullptr) layers_->groupSelected();
        });
        groupAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+G")));
        QAction* ungroupAct = layerMenu->addAction(QStringLiteral("&Ungroup Layers"), this, [this] {
            if (layers_ != nullptr) layers_->ungroupSelected();
        });
        ungroupAct->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+G")));

        // Layer Mask: a non-destructive raster mask on the active layer; the compositor multiplies
        // its coverage into the layer's alpha. Each action checks applicability BEFORE pushing, so
        // an inapplicable choice records no phantom undo entry and doesn't clear the redo branch
        // (the engine commands no-op too, but History::push would still truncate redo).
        layerMenu->addSeparator();
        auto* maskMenu = layerMenu->addMenu(QStringLiteral("Layer Mask"));
        const auto addMask = [this](pe::AddLayerMaskCommand::Init init) {
            if (doc_ == nullptr) return;
            const pe::Layer* l = doc_->findLayer(doc_->activeLayer());
            if (refuseIf(l == nullptr, "layer.mask.add", pe::RefusalCode::NoActiveLayer,
                         "Layer > Layer Mask",
                         QStringLiteral("Select a layer to add a mask to."))) {
                return;
            }
            if (refuseIf(l->mask() != nullptr, "layer.mask.add",
                         pe::RefusalCode::LayerAlreadyHasMask, "Layer > Layer Mask",
                         QStringLiteral("\"%1\" already has a mask. Delete it first to add a "
                                        "different one.")
                             .arg(QString::fromStdString(l->name())))) {
                return;
            }
            doc_->history().push(
                std::make_unique<pe::AddLayerMaskCommand>(doc_->activeLayer(), init));
        };
        maskMenu->addAction(QStringLiteral("Reveal All"), this,
                            [addMask] { addMask(pe::AddLayerMaskCommand::Init::RevealAll); });
        maskMenu->addAction(QStringLiteral("Hide All"), this,
                            [addMask] { addMask(pe::AddLayerMaskCommand::Init::HideAll); });
        maskMenu->addAction(QStringLiteral("From Selection"), this,
                            [addMask] { addMask(pe::AddLayerMaskCommand::Init::FromSelection); });
        maskMenu->addSeparator();
        maskMenu->addAction(QStringLiteral("Toggle Mask Enabled"), this, [this] {
            if (doc_ == nullptr) return;
            const pe::Layer* l = doc_->findLayer(doc_->activeLayer());
            if (refuseIf(l == nullptr, "layer.mask.toggle", pe::RefusalCode::NoActiveLayer,
                         "Layer > Layer Mask > Toggle Mask Enabled",
                         QStringLiteral("Select a layer first."))) {
                return;
            }
            if (refuseIf(l->mask() == nullptr, "layer.mask.toggle", pe::RefusalCode::LayerHasNoMask,
                         "Layer > Layer Mask > Toggle Mask Enabled",
                         QStringLiteral("\"%1\" has no mask to enable or disable.")
                             .arg(QString::fromStdString(l->name())))) {
                return;
            }
            doc_->history().push(std::make_unique<pe::SetMaskEnabledCommand>(
                doc_->activeLayer(), !l->mask()->enabled()));
        });
        maskMenu->addAction(QStringLiteral("Delete Mask"), this, [this] {
            if (doc_ == nullptr) return;
            const pe::Layer* l = doc_->findLayer(doc_->activeLayer());
            if (refuseIf(l == nullptr, "layer.mask.delete", pe::RefusalCode::NoActiveLayer,
                         "Layer > Layer Mask > Delete Mask",
                         QStringLiteral("Select a layer first."))) {
                return;
            }
            if (refuseIf(l->mask() == nullptr, "layer.mask.delete", pe::RefusalCode::LayerHasNoMask,
                         "Layer > Layer Mask > Delete Mask",
                         QStringLiteral("\"%1\" has no mask to delete.")
                             .arg(QString::fromStdString(l->name())))) {
                return;
            }
            doc_->history().push(std::make_unique<pe::RemoveLayerMaskCommand>(doc_->activeLayer()));
        });

        // New Adjustment Layer: a non-destructive layer that transforms the composite beneath it at
        // render time. Added on top of the stack as one undo step;
        // visibility/opacity/blend/reorder/ delete all work through the existing layer commands,
        // and editing its parameters arrives next.
        layerMenu->addSeparator();
        auto* adjMenu = layerMenu->addMenu(QStringLiteral("New Adjustment Layer"));
        using AdjFactory = std::unique_ptr<pe::Adjustment> (*)();
        const std::pair<const char*, AdjFactory> adjTypes[] = {
            {"Brightness/Contrast",
             []() -> std::unique_ptr<pe::Adjustment> {
                 return std::make_unique<pe::BrightnessContrast>();
             }},
            {"Levels",
             []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Levels>(); }},
            {"Curves",
             []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Curves>(); }},
            {"Exposure",
             []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Exposure>(); }},
            {"Vibrance",
             []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Vibrance>(); }},
            {"Hue/Saturation",
             []() -> std::unique_ptr<pe::Adjustment> {
                 return std::make_unique<pe::HueSaturation>();
             }},
            {"Color Balance",
             []() -> std::unique_ptr<pe::Adjustment> {
                 return std::make_unique<pe::ColorBalance>();
             }},
            {"Black & White",
             []() -> std::unique_ptr<pe::Adjustment> {
                 return std::make_unique<pe::BlackAndWhite>();
             }},
            {"Photo Filter",
             []() -> std::unique_ptr<pe::Adjustment> {
                 return std::make_unique<pe::PhotoFilter>();
             }},
            {"Channel Mixer",
             []() -> std::unique_ptr<pe::Adjustment> {
                 return std::make_unique<pe::ChannelMixer>();
             }},
            {"Invert",
             []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Invert>(); }},
            {"Posterize",
             []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Posterize>(); }},
            {"Threshold",
             []() -> std::unique_ptr<pe::Adjustment> { return std::make_unique<pe::Threshold>(); }},
            {"Gradient Map",
             []() -> std::unique_ptr<pe::Adjustment> {
                 return std::make_unique<pe::GradientMap>();
             }},
            {"Selective Color",
             []() -> std::unique_ptr<pe::Adjustment> {
                 return std::make_unique<pe::SelectiveColor>();
             }},
        };
        for (const auto& [label, make] : adjTypes) {
            adjMenu->addAction(QString::fromUtf8(label), this, [this, label, make] {
                addAdjustmentLayer(make(), QString::fromUtf8(label));
            });
        }
        // Edit the active adjustment layer's parameters (also reachable by double-clicking its
        // row).
        layerMenu->addAction(QStringLiteral("Edit Adjustment..."), this, [this] {
            if (doc_ == nullptr) return;
            const pe::Layer* a = doc_->findLayer(doc_->activeLayer());
            if (refuseIf(a == nullptr || !a->isAdjustment(), "layer.adjustment.edit",
                         a == nullptr ? pe::RefusalCode::NoActiveLayer
                                      : pe::RefusalCode::LayerNotAdjustment,
                         "Layer > Edit Adjustment...",
                         QStringLiteral("Select an adjustment layer to edit."))) {
                return;
            }
            editAdjustmentLayer(doc_->activeLayer());
        });
    }
    auto* selMenu = menuBar()->addMenu(QStringLiteral("&Select"));
    docMenus_.push_back(selMenu);
    selMenu->addAction(
        QStringLiteral("Select All"), standardOr(QKeySequence::SelectAll, "Ctrl+A"), this,
        [this]() {
            if (doc_) {
                Selection target;
                target.selectAll(doc_->canvasBounds());
                doc_->history().push(std::make_unique<SetSelectionCommand>(std::move(target)));
            }
        });
    selMenu->addAction(
        QStringLiteral("Deselect"), QKeySequence(QStringLiteral("Ctrl+D")), this, [this]() {
            if (doc_) {
                Selection target;
                target.selectNone();
                doc_->history().push(std::make_unique<SetSelectionCommand>(std::move(target)));
            }
        });
    selMenu->addSeparator();
    selMenu->addAction(
        QStringLiteral("Invert Selection"), QKeySequence(QStringLiteral("Ctrl+Shift+I")), this,
        [this]() {
            if (doc_) {
                Selection target = doc_->selection();
                target.invert(doc_->canvasBounds());
                doc_->history().push(std::make_unique<SetSelectionCommand>(std::move(target)));
            }
        });
    selMenu->addSeparator();
    // Edge refinements. Each prompts for an amount, applies it to a copy of the active selection,
    // and pushes the result as one undo step — but only when it actually changed the selection, so
    // a no-op (e.g. a region over the working cap) leaves no phantom undo entry.
    // The prompt is UI; the operation is not. Splitting them keeps the refusal path
    // reachable from a test without having to dismiss a modal dialog.
    selMenu->addAction(QStringLiteral("Grow..."), this, [this]() {
        bool ok = false;
        const int px =
            QInputDialog::getInt(this, QStringLiteral("Grow Selection"),
                                 QStringLiteral("Expand by (pixels):"), 4, 1, 1000, 1, &ok);
        if (ok) growSelection(px);
    });
    selMenu->addAction(QStringLiteral("Shrink..."), this, [this]() {
        bool ok = false;
        const int px =
            QInputDialog::getInt(this, QStringLiteral("Shrink Selection"),
                                 QStringLiteral("Contract by (pixels):"), 4, 1, 1000, 1, &ok);
        if (ok) shrinkSelection(px);
    });
    selMenu->addAction(QStringLiteral("Feather..."), this, [this]() {
        bool ok = false;
        const double r =
            QInputDialog::getDouble(this, QStringLiteral("Feather Selection"),
                                    QStringLiteral("Radius (pixels):"), 4.0, 0.1, 250.0, 1, &ok);
        if (ok) featherSelection(static_cast<float>(r));
    });
    auto* filterMenu = menuBar()->addMenu(QStringLiteral("F&ilter"));
    docMenus_.push_back(filterMenu);
    {
        filterMenu->addAction(QStringLiteral("Gaussian Blur..."), this, [this, runEffect] {
            runEffect(QStringLiteral("Gaussian Blur"),
                      {{QStringLiteral("Radius (sigma)"), 0.0, 20.0, 2.0, 1}},
                      [this](const std::vector<double>& v) {
                          return pe::applyFilter(*doc_, doc_->activeLayer(),
                                                 pe::GaussianBlurFilter(static_cast<float>(v[0])),
                                                 &doc_->selection());
                      });
        });
        filterMenu->addAction(QStringLiteral("Box Blur..."), this, [this, runEffect] {
            runEffect(QStringLiteral("Box Blur"), {{QStringLiteral("Radius"), 0.0, 50.0, 3.0, 0}},
                      [this](const std::vector<double>& v) {
                          return pe::applyFilter(*doc_, doc_->activeLayer(),
                                                 pe::BoxBlurFilter(static_cast<int>(v[0])),
                                                 &doc_->selection());
                      });
        });
        filterMenu->addAction(QStringLiteral("Sharpen..."), this, [this, runEffect] {
            runEffect(QStringLiteral("Sharpen (Unsharp Mask)"),
                      {{QStringLiteral("Radius"), 0.0, 20.0, 1.0, 1},
                       {QStringLiteral("Amount"), 0.0, 5.0, 1.0, 2},
                       {QStringLiteral("Threshold"), 0.0, 1.0, 0.0, 2}},
                      [this](const std::vector<double>& v) {
                          return pe::applyFilter(
                              *doc_, doc_->activeLayer(),
                              pe::SharpenFilter(static_cast<float>(v[0]), static_cast<float>(v[1]),
                                                static_cast<float>(v[2])),
                              &doc_->selection());
                      });
        });
        filterMenu->addAction(QStringLiteral("Mosaic..."), this, [this, runEffect] {
            runEffect(QStringLiteral("Mosaic"), {{QStringLiteral("Cell Size"), 1.0, 100.0, 8.0, 0}},
                      [this](const std::vector<double>& v) {
                          return pe::applyFilter(*doc_, doc_->activeLayer(),
                                                 pe::MosaicFilter(static_cast<int>(v[0])),
                                                 &doc_->selection());
                      });
        });
        filterMenu->addSeparator();
        filterMenu->addAction(QStringLiteral("Find Edges"), this, [this, applyInstant, sel] {
            if (doc_)
                applyInstant(
                    pe::applyFilter(*doc_, doc_->activeLayer(), pe::FindEdgesFilter{}, sel()));
        });
    }

    auto* viewMenu = menuBar()->addMenu(QStringLiteral("&View"));
    QAction* zoomInAct =
        viewMenu->addAction(QStringLiteral("Zoom &In"), canvas_, &CanvasView::zoomIn);
    zoomInAct->setShortcut(QKeySequence::ZoomIn);
    QAction* zoomOutAct =
        viewMenu->addAction(QStringLiteral("Zoom &Out"), canvas_, &CanvasView::zoomOut);
    zoomOutAct->setShortcut(QKeySequence::ZoomOut);
    viewMenu->addSeparator();
    QAction* fitAct =
        viewMenu->addAction(QStringLiteral("&Fit on Screen"), canvas_, &CanvasView::fitToWindow);
    fitAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    QAction* actualAct =
        viewMenu->addAction(QStringLiteral("&Actual Pixels"), canvas_, &CanvasView::actualPixels);
    actualAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_1));

    viewMenu->addSeparator();
    QMenu* themeMenu = viewMenu->addMenu(QStringLiteral("&Theme"));
    auto* themeGroup = new QActionGroup(this);
    // Labels come from themeName() so the menu cannot drift from the theme's own
    // name, and a new theme appears here without touching this loop.
    for (const ThemeId id : kAllThemes) {
        QAction* a = themeMenu->addAction(QString::fromUtf8(themeName(id)));
        a->setCheckable(true);
        a->setChecked(currentTheme() == id);
        themeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, id] { setTheme(id); });
    }

    // Filled by populateWindowMenu() once the docks exist; a dock's toggle action is
    // owned by the dock, so there is nothing to add until they are constructed.
    windowMenu_ = menuBar()->addMenu(QStringLiteral("&Window"));

    QMenu* helpMenu = menuBar()->addMenu(QStringLiteral("&Help"));
    helpMenu->addAction(QStringLiteral("&About PhotoEdit..."), this, &MainWindow::showAbout);
}

void MainWindow::populateWindowMenu() {
    if (windowMenu_ == nullptr) return;
    windowMenu_->clear();

    // Grouped to match the three stacked panel columns on the right, so the menu
    // reads in the same order the panels appear.
    const QStringList groups[] = {
        {QStringLiteral("Color"), QStringLiteral("Swatches"), QStringLiteral("Gradients"),
         QStringLiteral("Patterns")},
        {QStringLiteral("Properties"), QStringLiteral("Adjustments"), QStringLiteral("Libraries")},
        {QStringLiteral("Layers"), QStringLiteral("Channels"), QStringLiteral("Paths"),
         QStringLiteral("History")},
    };

    const QList<QDockWidget*> docks = findChildren<QDockWidget*>();
    bool firstGroup = true;
    for (const QStringList& group : groups) {
        if (!firstGroup) windowMenu_->addSeparator();
        firstGroup = false;
        for (const QString& title : group) {
            for (QDockWidget* d : docks) {
                if (d->objectName() != title) continue;
                // toggleViewAction() is checkable and already tracks visibility both
                // ways, so closing a panel unchecks its entry with no extra wiring.
                windowMenu_->addAction(d->toggleViewAction());
                break;
            }
        }
    }
}

void MainWindow::clearCursorPos() {
    // Placeholder rather than an empty string so the readout does not change width
    // as the pointer crosses the canvas edge.
    if (posLabel_ != nullptr) posLabel_->setText(QStringLiteral("X -  Y -"));
}

void MainWindow::refineSelection(const char* action,
                                 const std::function<void(pe::Selection&)>& apply) {
    if (doc_ == nullptr) return;
    if (refuseIf(!doc_->selection().active(), "select.refine", pe::RefusalCode::NoSelection, action,
                 QStringLiteral("There is no selection to refine. Select something first."))) {
        return;
    }
    pe::Selection target = doc_->selection();
    apply(target);
    // Refusing a no-op matters as much as refusing an impossible one: pushing it would add
    // a history entry the user then has to undo, and saying nothing leaves them believing
    // the amount they typed was too small to see.
    if (refuseIf(target == doc_->selection(), "select.refine", pe::RefusalCode::NoEffect, action,
                 QStringLiteral("That left the selection unchanged. Try a larger amount."))) {
        return;
    }
    doc_->history().push(std::make_unique<pe::SetSelectionCommand>(std::move(target)));
}

void MainWindow::growSelection(int px) {
    refineSelection("Select > Grow...", [px](pe::Selection& s) { s.grow(px); });
}

void MainWindow::shrinkSelection(int px) {
    refineSelection("Select > Shrink...", [px](pe::Selection& s) { s.shrink(px); });
}

void MainWindow::featherSelection(float radius) {
    if (doc_ == nullptr) return;
    const pe::Rect canvas = doc_->canvasBounds();
    refineSelection("Select > Feather...",
                    [radius, canvas](pe::Selection& s) { s.feather(radius, canvas); });
}

void MainWindow::reportRefusal(const pe::Refusal& r) {
    if (!r.isRefusal()) return;
    refusals_.push_back(r);
    // Shown for longer than an ordinary transient message: a refusal is the answer to
    // "why did nothing happen", and the user has to read it to get that answer.
    statusBar()->showMessage(QString::fromStdString(r.explanation), 6000);
}

TaskResult MainWindow::runGuardedTask(const QString& title, TaskAccess access,
                                      const std::function<void()>& work) {
    // Refuse rather than nest. A task already in flight holds a snapshot and a nested event
    // loop, and a second one started from inside it would run two workers against a
    // single-threaded engine. The File actions are disabled for the duration, so the UI does
    // not normally offer this; the canvas's own tools stay live during a snapshot task and
    // reach here through CanvasView's runner, which is the path that made this reachable.
    // An un-run TaskResult is what a caller already handles for work it did not perform.
    if (documentTaskInFlight_) return {};

    // The guard itself. It comes back however the task ends, including by exception.
    struct InFlight {
        MainWindow* window;
        explicit InFlight(MainWindow* w) : window(w) {
            window->documentTaskInFlight_ = true;
            window->updateActionStates();
        }
        ~InFlight() {
            window->documentTaskInFlight_ = false;
            window->updateActionStates();
        }
        InFlight(const InFlight&) = delete;
        InFlight& operator=(const InFlight&) = delete;
    } inFlight(this);

    return runDocumentTask(this, canvas_, title, access, work);
}

bool MainWindow::refuseIf(bool condition, const char* operation, pe::RefusalCode code,
                          const char* action, const QString& explanation) {
    if (!condition) return false;
    reportRefusal(pe::refuse(operation, code, action, explanation.toStdString(),
                             describeState().toStdString()));
    return true;
}

QString MainWindow::describeState() const {
    // The context field: enough to diagnose a refusal from a report, without the reporter
    // having to reproduce it.
    if (doc_ == nullptr) return QStringLiteral("no document");
    const pe::Layer* active = doc_->findLayer(doc_->activeLayer());
    const QString layer =
        active == nullptr
            ? QStringLiteral("none")
            : QStringLiteral("%1 (%2%3)")
                  .arg(QString::fromStdString(active->name()))
                  .arg(active->kind() == pe::LayerKind::Group   ? QStringLiteral("group")
                       : active->isAdjustment()                 ? QStringLiteral("adjustment")
                       : active->kind() == pe::LayerKind::Pixel ? QStringLiteral("pixel")
                                                                : QStringLiteral("other"))
                  .arg(active->mask() != nullptr ? QStringLiteral(", masked") : QString());
    return QStringLiteral("canvas %1x%2; active layer %3; selection %4; %5 undo step(s)")
        .arg(doc_->canvasSize().width)
        .arg(doc_->canvasSize().height)
        .arg(layer)
        .arg(doc_->selection().active() ? QStringLiteral("active") : QStringLiteral("none"))
        .arg(doc_->history().undoDepth());
}

void MainWindow::updateActionStates() {
    const bool hasDoc = doc_ != nullptr;
    // A snapshot save leaves the canvas live on purpose, so the user keeps painting. What
    // must NOT happen meanwhile is another file operation: the running one finishes by
    // touching this document's history and path, and New/Open/Exit would have replaced or
    // destroyed it. Disabling the actions (rather than the menu) also disables their
    // shortcuts, which is what a keyboard user would otherwise reach them by.
    const bool idle = !documentTaskInFlight_;
    for (QMenu* m : docMenus_) {
        if (m != nullptr) m->setEnabled(hasDoc);
    }
    // Every preset adds a layer to a document, so with none open the panel is greyed rather
    // than left looking live and doing nothing when clicked. Same rule the menus follow.
    if (adjustments_ != nullptr) adjustments_->setEnabled(hasDoc);
    for (QAction* a : fileActions_) {
        if (a != nullptr) a->setEnabled(idle);
    }
    for (QAction* a : docActions_) {
        if (a != nullptr) a->setEnabled(hasDoc && idle);
    }
    // Undo and Redo track the history rather than merely the document, so they grey
    // out at the ends of the stack instead of silently doing nothing.
    if (undoAct_ != nullptr) undoAct_->setEnabled(hasDoc && doc_->history().canUndo());
    if (redoAct_ != nullptr) redoAct_->setEnabled(hasDoc && doc_->history().canRedo());
    // Clipping is a per-layer state, not a one-way action, so the entry carries a checkmark
    // that follows the ACTIVE layer. Left stale it would report the previous layer's state.
    if (clipAct_ != nullptr) {
        clipAct_->setChecked(hasDoc && layers_ != nullptr && layers_->activeIsClipped());
    }
}

void MainWindow::showAbout() {
    QMessageBox::about(
        this, QStringLiteral("About PhotoEdit"),
        QStringLiteral("<h3>PhotoEdit %1</h3>"
                       "<p>A color-managed, tile-based image editor.</p>"
                       "<p>Engine %1 &middot; Qt %2</p>")
            .arg(QString::fromUtf8(pe::Version::string()), QString::fromUtf8(qVersion())));
}

void MainWindow::buildToolBar() {
    auto* tb = new QToolBar(QStringLiteral("Tools"), this);
    tb->setObjectName(QStringLiteral("ToolStrip"));
    tb->setMovable(false);
    tb->setFloatable(false);
    tb->setIconSize(QSize(22, 22));
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    addToolBar(Qt::LeftToolBarArea, tb);

    // Grouped like a pro editor: select · crop/sample · paint · type · navigate.
    // Brush/Eraser/Hand/Zoom/Marquee/Eyedropper/Move wired; others scaffolded.
    using Tool = CanvasView::Tool;
    const std::vector<std::vector<ToolDef>> groups = {
        {{"move", "Move", Tool::Move, "V", "Drag to move the active layer"},
         {"marquee", "Rectangular Marquee", Tool::Marquee, "M",
          "Drag to select a rectangle. Shift adds, Alt subtracts"},
         {"lasso", "Lasso", Tool::Lasso, "L", "Drag to draw a freehand selection"},
         {"wand-sparkles", "Magic Wand", Tool::Wand, "W",
          "Click to select a similarly colored region"}},
        {{"crop", "Crop", Tool::Crop, "C", "Drag to set the crop, then release to apply"},
         {"frame", "Frame", Tool::Inactive, "K", ""},
         {"pipette", "Eyedropper", Tool::Eyedropper, "I",
          "Click to pick up a color from the canvas"}},
        {{"bandage", "Spot Healing Brush", Tool::Heal, "J",
          "Drag over a blemish to blend it into its surroundings"},
         {"paintbrush", "Brush", Tool::Brush, "B", "Drag to paint with the foreground color"},
         {"stamp", "Clone Stamp", Tool::Clone, "S",
          "Alt-click to set a source, then drag to clone"},
         {"history", "History Brush", Tool::Inactive, "Y", ""},
         {"eraser", "Eraser", Tool::Eraser, "E", "Drag to erase to transparency"},
         {"blend", "Gradient", Tool::Gradient, "G",
          "Drag to draw a gradient from the foreground to the background color"},
         {"paint-bucket", "Paint Bucket", Tool::Bucket, "",
          "Click to fill a similarly colored region"}},
        {{"droplet", "Blur", Tool::Blur, "", "Drag to soften detail. Hold Alt to sharpen"},
         {"sun", "Dodge", Tool::Dodge, "O", "Drag to lighten. Hold Alt to burn"}},
        {{"pen-tool", "Pen", Tool::Inactive, "P", ""},
         {"type", "Type", Tool::Type, "T", "Click on the canvas to place text"},
         {"mouse-pointer-2", "Path Selection", Tool::Inactive, "A", ""},
         {"shapes", "Shape", Tool::Inactive, "U", ""}},
        {{"hand", "Hand", Tool::Hand, "H", "Drag to pan the view"},
         {"zoom-in", "Zoom", Tool::Zoom, "Z", "Click to zoom in. Alt-click to zoom out"}},
    };

    auto* toolGroup = new QActionGroup(this);
    toolGroup_ = toolGroup;
    QAction* brushAction = nullptr;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        if (g > 0) tb->addSeparator();
        for (const ToolDef& def : groups[g]) {
            QAction* a =
                tb->addAction(renderIconAsIcon(QString::fromUtf8(def.icon), themeIconColor(), 22),
                              QString::fromUtf8(def.label));
            themedIcons_.push_back({a, QString::fromUtf8(def.icon), 22});
            a->setCheckable(true);
            a->setActionGroup(toolGroup);
            const bool wired = def.tool != Tool::Inactive;
            const QString shortcut = QString::fromUtf8(def.shortcut);
            // The hint is the single source for "what does this tool do"; it feeds both
            // the tooltip here and the status bar on selection, so the two cannot drift.
            const QString hint = QString::fromUtf8(def.hint);
            QString tip = QString::fromUtf8(def.label);
            if (!shortcut.isEmpty()) tip += QStringLiteral("  (%1)").arg(shortcut);
            if (!hint.isEmpty()) tip += QStringLiteral("\n%1").arg(hint);
            if (!wired) tip += QStringLiteral("\nNot yet implemented");
            a->setToolTip(tip);
            if (!shortcut.isEmpty()) a->setShortcut(QKeySequence(shortcut));
            const Tool tool = def.tool;
            const QString label = QString::fromUtf8(def.label);
            OptKind kind = OptKind::None;
            if (def.tool == Tool::Brush || def.tool == Tool::Eraser || def.tool == Tool::Dodge ||
                def.tool == Tool::Clone || def.tool == Tool::Blur || def.tool == Tool::Heal) {
                kind = OptKind::Brush;  // size/opacity drive the brush footprint + strength
            } else if (def.tool == Tool::Wand) {
                kind = OptKind::Wand;  // tolerance drives the magic-wand flood
            } else if (def.tool == Tool::Move) {
                kind = OptKind::Move;  // auto-select and the on-canvas transform box
            }
            // Only wired tools, and only the first action for a given tool: Tool::Inactive
            // covers several buttons and none of them selects anything.
            if (wired && !toolActions_.contains(static_cast<int>(tool))) {
                toolActions_.insert(static_cast<int>(tool), a);
            }
            connect(a, &QAction::triggered, this, [this, tool, label, wired, kind, hint] {
                canvas_->setTool(tool);
                toolLabel_->setText(label);
                // The hint answers "what do I do with this", which the bare tool name
                // never did. Scaffolded tools say so here instead of in the name.
                if (toolHintLabel_ != nullptr) {
                    toolHintLabel_->setText(wired ? hint : QStringLiteral("Not yet implemented"));
                }
                updateOptionsBar(kind, label);
            });
            if (def.tool == Tool::Brush) brushAction = a;
        }
    }
    if (brushAction != nullptr) brushAction->setChecked(true);  // default tool

    // Foreground / background colour swatches anchored at the bottom of the strip.
    auto* spacer = new QWidget(tb);
    spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    tb->addWidget(spacer);
    tb->addSeparator();
    tb->addWidget(makeColorSwatches());
}

void MainWindow::buildOptionsBar() {
    optionsBar_ = new QToolBar(QStringLiteral("Options"), this);
    optionsBar_->setObjectName(QStringLiteral("OptionsBar"));
    optionsBar_->setMovable(false);
    optionsBar_->setFloatable(false);
    addToolBar(Qt::TopToolBarArea, optionsBar_);

    optToolName_ = new QLabel(optionsBar_);
    optToolName_->setObjectName(QStringLiteral("OptToolName"));
    optionsBar_->addWidget(optToolName_);
    optionsBar_->addSeparator();

    // Brush/eraser options (size + opacity); shown only for paint tools.
    brushOptions_ = new QWidget(optionsBar_);
    auto* bl = new QHBoxLayout(brushOptions_);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(6);
    bl->addWidget(new QLabel(QStringLiteral("Size"), brushOptions_));
    sizeSpin_ = new QSpinBox(brushOptions_);
    sizeSpin_->setObjectName(QStringLiteral("BrushSize"));
    sizeSpin_->setRange(1, 500);
    sizeSpin_->setValue(static_cast<int>(canvas_->tool().brush().diameter));
    sizeSpin_->setSuffix(QStringLiteral(" px"));
    bl->addWidget(sizeSpin_);
    bl->addWidget(new QLabel(QStringLiteral("Opacity"), brushOptions_));
    opacitySpinOpt_ = new QSpinBox(brushOptions_);
    opacitySpinOpt_->setObjectName(QStringLiteral("BrushOpacity"));
    opacitySpinOpt_->setRange(1, 100);
    opacitySpinOpt_->setValue(static_cast<int>(canvas_->tool().brush().opacity * 100.0f));
    opacitySpinOpt_->setSuffix(QStringLiteral("%"));
    bl->addWidget(opacitySpinOpt_);
    bl->addWidget(new QLabel(QStringLiteral("Flow"), brushOptions_));
    flowSpin_ = new QSpinBox(brushOptions_);
    flowSpin_->setRange(1, 100);
    flowSpin_->setValue(static_cast<int>(canvas_->tool().brush().flow * 100.0f));
    flowSpin_->setSuffix(QStringLiteral("%"));
    bl->addWidget(flowSpin_);

    // Brush dynamics UI skeleton: stabilization (0-100%)
    bl->addWidget(new QLabel(QStringLiteral("Stabilize"), brushOptions_));
    stabSpin_ = new QSpinBox(brushOptions_);
    stabSpin_->setRange(0, 100);
    stabSpin_->setValue(static_cast<int>(canvas_->tool().brush().stabilize * 100.0f));
    stabSpin_->setSuffix(QStringLiteral("%"));
    bl->addWidget(stabSpin_);
    brushOptAction_ = optionsBar_->addWidget(brushOptions_);
    brushOptAction_->setObjectName(QStringLiteral("BrushOptionsAction"));

    connect(sizeSpin_, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->tool().brush().diameter = static_cast<float>(v); });
    connect(opacitySpinOpt_, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->tool().brush().opacity = static_cast<float>(v) / 100.0f; });
    connect(flowSpin_, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->tool().brush().flow = static_cast<float>(v) / 100.0f; });
    connect(stabSpin_, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->tool().brush().stabilize = static_cast<float>(v) / 100.0f; });
    // Brush settings are per tool, so a tool change swaps them under these boxes.
    connect(canvas_, &CanvasView::brushSettingsSwapped, this, &MainWindow::refreshBrushOptions);

    // Magic-wand options — per-channel tolerance for the flood; shown only for the Wand tool.
    wandOptions_ = new QWidget(optionsBar_);
    auto* wl = new QHBoxLayout(wandOptions_);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->setSpacing(6);
    wl->addWidget(new QLabel(QStringLiteral("Tolerance"), wandOptions_));
    wandTolSpin_ = new QSpinBox(wandOptions_);
    wandTolSpin_->setRange(0, 255);
    wandTolSpin_->setValue(32);
    wl->addWidget(wandTolSpin_);
    wandOptAction_ = optionsBar_->addWidget(wandOptions_);
    wandOptAction_->setObjectName(QStringLiteral("WandOptionsAction"));
    wandOptAction_->setVisible(false);

    connect(wandTolSpin_, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->setWandTolerance(v); });

    // Move options. Auto-Select picks the layer under the cursor when a drag starts; the
    // combo beside it chooses whether that means the individual layer or the top-level group
    // containing it. Show Transform Controls draws the active layer's box and handles, and
    // grabbing one enters Free Transform. All three are connected; the group that used to sit
    // here was a scaffold with no connect() at all, which is why it was removed (#133).
    moveOptions_ = new QWidget(optionsBar_);
    auto* mvl = new QHBoxLayout(moveOptions_);
    mvl->setContentsMargins(0, 0, 0, 0);
    mvl->setSpacing(6);

    auto* autoSelectBox = new QCheckBox(QStringLiteral("Auto-Select"), moveOptions_);
    autoSelectBox->setObjectName(QStringLiteral("MoveAutoSelect"));
    autoSelectBox->setToolTip(
        QStringLiteral("Start the drag on the layer under the cursor, instead of on the active "
                       "layer"));
    autoSelectBox->setAccessibleName(QStringLiteral("Auto-Select"));
    mvl->addWidget(autoSelectBox);

    auto* autoSelectMode = new QComboBox(moveOptions_);
    autoSelectMode->setObjectName(QStringLiteral("MoveAutoSelectMode"));
    autoSelectMode->addItem(QStringLiteral("Layer"));
    autoSelectMode->addItem(QStringLiteral("Group"));
    autoSelectMode->setToolTip(
        QStringLiteral("What Auto-Select picks: the layer itself, or the top-level group "
                       "containing it"));
    autoSelectMode->setAccessibleName(QStringLiteral("Auto-Select granularity"));
    autoSelectMode->setEnabled(false);  // it decides nothing until Auto-Select is on
    mvl->addWidget(autoSelectMode);

    auto* showTransformBox = new QCheckBox(QStringLiteral("Show Transform Controls"), moveOptions_);
    showTransformBox->setObjectName(QStringLiteral("MoveShowTransform"));
    showTransformBox->setToolTip(
        QStringLiteral("Draw the active layer's bounding box. Drag a corner or the rotate knob "
                       "to start a Free Transform"));
    showTransformBox->setAccessibleName(QStringLiteral("Show Transform Controls"));
    mvl->addWidget(showTransformBox);

    moveOptAction_ = optionsBar_->addWidget(moveOptions_);
    // Named, like the tree and the panels are: which option group the bar is showing is
    // the observable that says the bar is contextual at all, and a hidden toolbar widget
    // cannot be asked in a window that was never shown.
    moveOptAction_->setObjectName(QStringLiteral("MoveOptionsAction"));
    moveOptAction_->setVisible(false);

    connect(autoSelectBox, &QCheckBox::toggled, this, [this, autoSelectMode](bool on) {
        canvas_->setAutoSelect(on);
        autoSelectMode->setEnabled(on);
    });
    connect(autoSelectMode, &QComboBox::currentIndexChanged, this, [this](int index) {
        canvas_->setAutoSelectMode(index == 1 ? CanvasView::AutoSelectMode::Group
                                              : CanvasView::AutoSelectMode::Layer);
    });
    connect(showTransformBox, &QCheckBox::toggled, this,
            [this](bool on) { canvas_->setShowTransformControls(on); });

    // Right-aligned utility icons. These are not wired to anything yet, so they say
    // so on click and carry the same "coming soon" hint the scaffolded tools use.
    // They previously took their tooltip straight from the icon filename, which put
    // the string "share-2" in front of the user.
    struct UtilDef {
        const char* icon;
        const char* label;
    };
    static constexpr UtilDef kUtilities[] = {
        {"search", "Search"},
        {"share-2", "Share"},
        {"cloud", "Cloud Documents"},
        {"settings", "Preferences"},
    };

    auto* rspacer = new QWidget(optionsBar_);
    rspacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    optionsBar_->addWidget(rspacer);

    // Canvas size and a working zoom control. The zoom level was previously a dead
    // label in the status bar: it reported the value but offered no way to change it,
    // so zooming was reachable only from the View menu or the wheel.
    auto* zoomGroup = new QWidget(optionsBar_);
    zoomGroup->setObjectName(QStringLiteral("ZoomGroup"));
    auto* zl = new QHBoxLayout(zoomGroup);
    zl->setContentsMargins(0, 0, 0, 0);
    zl->setSpacing(4);

    canvasSizeLabel_ = new QLabel(zoomGroup);
    canvasSizeLabel_->setObjectName(QStringLiteral("CanvasSize"));
    zl->addWidget(canvasSizeLabel_);

    auto makeZoomButton = [this, zoomGroup](const QString& text, const QString& name,
                                            const QString& tip, void (CanvasView::*slot)()) {
        auto* b = new QToolButton(zoomGroup);
        b->setObjectName(name);
        b->setText(text);
        b->setAutoRaise(true);
        b->setToolTip(tip);
        b->setAccessibleName(tip);  // text is a bare glyph, so it is not a usable name
        // Connect straight to the canvas rather than through a lambda capturing a
        // pointer-to-member: it is simpler, it ties the connection's lifetime to the
        // canvas that actually serves it, and it avoids routing a member pointer
        // through Qt's inlined functor machinery, which GCC flagged as possibly
        // uninitialized and UBSan reported as an invalid vptr under the sanitizer lane.
        connect(b, &QToolButton::clicked, canvas_, slot);
        return b;
    };

    zl->addWidget(makeZoomButton(QStringLiteral("−"), QStringLiteral("ZoomOut"),
                                 QStringLiteral("Zoom out"), &CanvasView::zoomOut));
    zoomValueLabel_ = new QLabel(zoomGroup);
    zoomValueLabel_->setObjectName(QStringLiteral("ZoomValue"));
    zoomValueLabel_->setAlignment(Qt::AlignCenter);
    zoomValueLabel_->setMinimumWidth(52);  // stops the row shifting as digits change
    zl->addWidget(zoomValueLabel_);
    zl->addWidget(makeZoomButton(QStringLiteral("+"), QStringLiteral("ZoomIn"),
                                 QStringLiteral("Zoom in"), &CanvasView::zoomIn));
    zl->addWidget(makeZoomButton(QStringLiteral("Fit"), QStringLiteral("ZoomFit"),
                                 QStringLiteral("Fit on screen"), &CanvasView::fitToWindow));

    optionsBar_->addWidget(zoomGroup);
    refreshZoomStrip();

    for (const UtilDef& def : kUtilities) {
        const QString label = QString::fromUtf8(def.label);
        auto* b = new QToolButton(optionsBar_);
        b->setIcon(renderIconAsIcon(QString::fromUtf8(def.icon), themeIconColor(), 18));
        themedButtons_.push_back({b, QString::fromUtf8(def.icon), 18});
        b->setAutoRaise(true);
        b->setToolTip(label + QStringLiteral("  (coming soon)"));
        // Icon-only buttons carry no text, so assistive technology has nothing to
        // announce without this.
        b->setAccessibleName(label);
        b->setAccessibleDescription(QStringLiteral("Not yet implemented"));
        connect(b, &QToolButton::clicked, this, [this, label] {
            statusBar()->showMessage(label + QStringLiteral(" is not yet implemented"), 4000);
        });
        optionsBar_->addWidget(b);
    }
}

void MainWindow::buildCentral() {
    auto* central = new QWidget(this);
    auto* v = new QVBoxLayout(central);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto* tabStrip = new QWidget(central);
    tabStrip->setObjectName(QStringLiteral("DocTabStrip"));
    auto* h = new QHBoxLayout(tabStrip);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    docTab_ = new QLabel(tabStrip);
    docTab_->setObjectName(QStringLiteral("DocTab"));
    h->addWidget(docTab_);
    h->addStretch(1);

    v->addWidget(tabStrip);
    v->addWidget(canvas_, 1);
    setCentralWidget(central);
    refreshDocTab();
}

void MainWindow::refreshBrushOptions() {
    // Blocked, because setValue would otherwise fire valueChanged and write the value
    // straight back into the settings it was just read from. Harmless today, and exactly
    // the loop that bites when one of these gains a side effect.
    const auto show = [](QSpinBox* box, int value) {
        if (box == nullptr) return;
        const QSignalBlocker blocked(box);
        box->setValue(value);
    };
    const pe::BrushSettings& b = canvas_->tool().brush();
    show(sizeSpin_, static_cast<int>(std::lround(b.diameter)));
    show(opacitySpinOpt_, static_cast<int>(std::lround(b.opacity * 100.0f)));
    show(flowSpin_, static_cast<int>(std::lround(b.flow * 100.0f)));
    show(stabSpin_, static_cast<int>(std::lround(b.stabilize * 100.0f)));
}

void MainWindow::updateOptionsBar(OptKind kind, const QString& toolName) {
    if (optToolName_ != nullptr) optToolName_->setText(toolName);
    // Toggle the toolbar ACTIONS, not the inner widgets — QToolBar lays widgets out
    // via their wrapping action, so hiding the widget alone leaves a gap/ghost.
    if (brushOptAction_ != nullptr) brushOptAction_->setVisible(kind == OptKind::Brush);
    if (wandOptAction_ != nullptr) wandOptAction_->setVisible(kind == OptKind::Wand);
    if (moveOptAction_ != nullptr) moveOptAction_->setVisible(kind == OptKind::Move);
}

void MainWindow::refreshZoomStrip() {
    const bool hasDoc = doc_ != nullptr;
    if (canvasSizeLabel_ != nullptr) {
        canvasSizeLabel_->setText(hasDoc ? QStringLiteral("%1 × %2 px")
                                               .arg(doc_->canvasSize().width)
                                               .arg(doc_->canvasSize().height)
                                         : QString());
    }
    if (zoomValueLabel_ != nullptr) {
        zoomValueLabel_->setText(
            hasDoc ? QStringLiteral("%1%").arg(canvas_->zoomPercent(), 0, 'f', 1) : QString());
    }
}

void MainWindow::refreshDocTab() {
    if (docTab_ == nullptr) return;
    if (doc_ == nullptr) {
        docTab_->setText(QStringLiteral("   No document   "));
        return;
    }
    const QString name =
        currentPath_.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(currentPath_).fileName();
    // Colour mode and depth come from the document. This line previously ended in the
    // literal string "RGB", so a Grayscale or 16-bit document described itself wrongly.
    // Zoom moved to the options-bar control, which can actually change it.
    docTab_->setText(QStringLiteral("   %1   %2 × %3 · %4/%5   ")
                         .arg(name)
                         .arg(doc_->canvasSize().width)
                         .arg(doc_->canvasSize().height)
                         .arg(QString::fromUtf8(colorModeName(doc_->colorMode())))
                         .arg(bitDepthBits(doc_->bitDepth())));
}

QWidget* MainWindow::makeColorSwatches() {
    auto* w = new QWidget;
    w->setFixedSize(40, 40);
    bgSwatch_ = new QToolButton(w);
    bgSwatch_->setGeometry(15, 14, 22, 22);
    bgSwatch_->setToolTip(QStringLiteral("Background color"));
    fgSwatch_ = new QToolButton(w);
    fgSwatch_->setGeometry(3, 2, 22, 22);  // overlaps the bg swatch, as in Photoshop
    fgSwatch_->setToolTip(QStringLiteral("Foreground color"));
    connect(fgSwatch_, &QToolButton::clicked, this, &MainWindow::chooseForegroundColor);
    connect(bgSwatch_, &QToolButton::clicked, this, &MainWindow::chooseBackgroundColor);
    updateSwatches();
    return w;
}

void MainWindow::updateSwatches() {
    const auto style = [](const QColor& c) {
        return QStringLiteral(
                   "QToolButton{background:%1;border:1px solid #0d0d0d;border-radius:2px;}"
                   "QToolButton:hover{border:1px solid #aaaaaa;}")
            .arg(c.name());
    };
    if (fgSwatch_ != nullptr) fgSwatch_->setStyleSheet(style(fgColor_));
    if (bgSwatch_ != nullptr) bgSwatch_->setStyleSheet(style(bgColor_));
    // Two of the gradient presets follow these colours, so their swatches are stale the moment
    // either changes. Every path that alters a colour ends up here, which is why it hangs off
    // this rather than off setForegroundColor alone (the background has its own setter).
    if (gradients_ != nullptr) gradients_->setColors(fgColor_, bgColor_);
}

void MainWindow::setForegroundColor(const QColor& c) {
    if (!c.isValid()) return;
    fgColor_ = c;
    canvas_->tool().setColor(pe::Rgbaf{static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
                                       static_cast<float>(c.blueF()), 1.0f});
    // setColor on both panels deliberately does not echo back, so this cannot recurse.
    if (colorPanel_ != nullptr) colorPanel_->setColor(fgColor_);
    if (swatchesPanel_ != nullptr) swatchesPanel_->setCurrentColor(fgColor_);
    updateSwatches();
}

void MainWindow::onColorPicked(const QColor& c) {
    if (!c.isValid()) return;
    setForegroundColor(c);
    statusBar()->showMessage(QStringLiteral("Picked %1").arg(c.name()), 2000);
}

void MainWindow::chooseForegroundColor() {
    setForegroundColor(QColorDialog::getColor(fgColor_, this, QStringLiteral("Foreground Color")));
}

void MainWindow::chooseBackgroundColor() {
    const QColor c = QColorDialog::getColor(bgColor_, this, QStringLiteral("Background Color"));
    if (!c.isValid()) return;
    bgColor_ = c;
    canvas_->setBackgroundColor(bgColor_);  // the Gradient tool's far stop tracks the bg swatch
    updateSwatches();
}

void MainWindow::newDocument() {
    if (!confirmDiscard()) return;
    setDocument(pe::Document::createBlank(pe::Size{800, 600}), QString());
    statusBar()->showMessage(QStringLiteral("New 800x600 document"), 3000);
}

void MainWindow::openDocument() {
    if (!confirmDiscard()) return;
    const QString path =
        QFileDialog::getOpenFileName(this, QStringLiteral("Open Image"), QString(), kOpenFilter);
    if (path.isEmpty()) return;

    pe::LoadError loadErr = pe::LoadError::None;
    std::unique_ptr<pe::Document> doc;
    // Off the GUI thread: decoding a 24 MP file takes about 600 ms, and a 512 MB one takes
    // far longer. Detached rather than Snapshot: loading builds a separate document and
    // never touches the one on screen, so the canvas keeps compositing, but input stays
    // blocked because the document the user would be editing is about to be replaced.
    const TaskResult task = runGuardedTask(
        QStringLiteral("Opening %1").arg(QFileInfo(path).fileName()), TaskAccess::Detached,
        [&doc, &path, &loadErr] { doc = pe::loadDocument(path.toStdString(), &loadErr); });
    // Refused as re-entrant (see runGuardedTask): the work never ran, so there is no
    // failure to report. Saying nothing is right here; the File actions are disabled while
    // a task is in flight, so reaching this is a programming error rather than a user one,
    // and a "Open failed" box would describe something that did not happen.
    if (!task.ran && !task.threw) return;
    if (task.threw) {
        QMessageBox::warning(
            this, QStringLiteral("Open failed"),
            QStringLiteral("Reading \"%1\" stopped with an error: %2").arg(path, task.error));
        return;
    }
    if (doc == nullptr) {
        QMessageBox::warning(this, QStringLiteral("Open failed"), openFailureReason(path, loadErr));
        return;
    }
    // A freshly loaded document is at a "saved" state for dirty tracking.
    doc->history().markSaved();
    setDocument(std::move(doc), path);
    statusBar()->showMessage(QStringLiteral("Opened %1").arg(path), 3000);
}

QString saveFailureReason(const pe::Document* doc, const QString& path, pe::SaveError err) {
    switch (err) {
        case pe::SaveError::TooLargeToFlatten: {
            // The native .pedoc format serializes tiles directly, so it has no flatten
            // limit and is the way out of this one.
            const pe::Size canvas = doc != nullptr ? doc->canvasSize() : pe::Size{0, 0};
            const int64_t area = static_cast<int64_t>(canvas.width) * canvas.height;
            return QStringLiteral(
                       "\"%1\" is %2 x %3 (%4 megapixels). Flattening to a raster format is "
                       "limited to %5 megapixels.\n\nSave as .pedoc to keep the full document.")
                .arg(path)
                .arg(canvas.width)
                .arg(canvas.height)
                .arg(area / 1'000'000)
                .arg(pe::kMaxCompositeImagePixels / 1'000'000);
        }
        case pe::SaveError::ExceedsFormatLimit: {
            // Not a memory limit, so do not offer .pedoc or a bigger budget as the way out:
            // the format itself cannot describe a side this long, and never will.
            const pe::Size c = doc != nullptr ? doc->canvasSize() : pe::Size{0, 0};
            return QStringLiteral(
                       "\"%1\" is %2 x %3, and that format cannot store a side longer than %4 "
                       "pixels. That is a limit of the file format itself, not of this "
                       "computer, so a smaller image or a different format is the only way "
                       "round it. PNG and TIFF have no such limit.")
                .arg(path)
                .arg(c.width)
                .arg(c.height)
                .arg(pe::kMaxWebpDimension);
        }
        case pe::SaveError::ContentOutOfRange:
            return QStringLiteral(
                       "A layer has content further than %1 pixels from the canvas, which is past "
                       "what a .pedoc can store. Nothing was written and the document is "
                       "unchanged. "
                       "Move that layer back toward the canvas, or delete it, and save again.")
                .arg(pe::kMaxCanvasDimension);
        case pe::SaveError::UnsupportedFormat:
            return QStringLiteral("\"%1\" has no extension this build can write.").arg(path);
        case pe::SaveError::CodecUnavailable:
            return QStringLiteral(
                "This build cannot write that format. Save as .pedoc, or use a build "
                "with the codec compiled in.");
        case pe::SaveError::CannotCreate:
            return QStringLiteral(
                       "Could not create a file next to \"%1\". Check that the folder exists, "
                       "is not read-only, and that you have permission to write to it.")
                .arg(path);
        case pe::SaveError::WriteFailed:
            return QStringLiteral(
                       "Writing \"%1\" failed partway. The most likely cause is a full disk. "
                       "Your previous file, if any, is untouched.")
                .arg(path);
        case pe::SaveError::ReplaceFailed:
            return QStringLiteral(
                       "\"%1\" was written but could not replace the existing file, which is "
                       "usually another program holding it open. Close it and try again; the "
                       "existing file is untouched.")
                .arg(path);
        case pe::SaveError::None:
            break;
    }
    return QStringLiteral("Could not save \"%1\".").arg(path);
}

QString openFailureReason(const QString& path, pe::LoadError err) {
    switch (err) {
        case pe::LoadError::UnsupportedFormat:
            return QStringLiteral("\"%1\" is not a format this build can open.").arg(path);
        case pe::LoadError::NotFound:
            return QStringLiteral("\"%1\" no longer exists. It may have been moved or deleted.")
                .arg(path);
        case pe::LoadError::PermissionDenied:
            return QStringLiteral(
                       "\"%1\" exists but could not be opened. Check that you have permission "
                       "to read it and that no other program has it locked.")
                .arg(path);
        case pe::LoadError::TooLarge:
            return QStringLiteral("\"%1\" is larger than the %2 MB this build will read.")
                .arg(path)
                .arg(512);
        case pe::LoadError::Truncated:
            return QStringLiteral("\"%1\" ended early. The file is incomplete or damaged.")
                .arg(path);
        case pe::LoadError::DecodeFailed:
            return QStringLiteral(
                       "\"%1\" could not be decoded. The contents do not match its extension, "
                       "or the file is damaged.")
                .arg(path);
        case pe::LoadError::None:
            break;
    }
    return QStringLiteral("Could not open \"%1\".").arg(path);
}

QString noAdjustmentEditorReason(pe::AdjustmentKind kind, const QString& name) {
    if (kind == pe::AdjustmentKind::Invert) {
        // Not a gap: inverting takes no parameters at all. The layer can still be hidden,
        // faded with opacity, masked, moved or deleted, so say what CAN be done with it
        // rather than leaving a dead end.
        return QStringLiteral(
                   "“%1” has no settings to change: it inverts every "
                   "channel. Use the layer's opacity, blend mode or a mask to "
                   "control how much of it shows.")
            .arg(name);
    }
    return QStringLiteral("Editing “%1” isn't supported yet.").arg(name);
}

bool MainWindow::saveDocument() {
    if (currentPath_.isEmpty()) return saveDocumentAs();
    return writeTo(currentPath_);
}

bool MainWindow::saveDocumentAs() {
    if (doc_ == nullptr) return false;
    const QString path =
        QFileDialog::getSaveFileName(this, QStringLiteral("Save As"), QString(), kSaveFilter);
    if (path.isEmpty()) return false;
    return writeTo(path);
}

bool MainWindow::writeTo(const QString& path) {
    if (doc_ == nullptr) return false;
    // Serialize an immutable SNAPSHOT rather than the live document. The snapshot shares
    // tile buffers copy-on-write, so it costs pointer copies rather than the document's
    // pixels, and the live document forks a tile before its next write. That is what lets
    // the canvas stay live through a save that used to take 3.3 seconds with the window
    // unable to repaint: the user keeps painting, and the file holds the document as it
    // was at the moment they asked for it.
    const std::unique_ptr<const pe::Document> shot = doc_->snapshot();
    if (shot == nullptr) return false;
    // Which state the file will represent. Recorded INSIDE the history rather than held here
    // as a depth: undo, a new branch and history trimming all reindex the stack while the
    // worker writes, so a number taken now would quietly come to mean a different state by
    // the time the bytes are on disk, and committing it would mark unsaved work clean.
    const std::uint64_t savePoint = doc_->history().beginSave();

    pe::SaveError saveErr = pe::SaveError::None;
    bool wrote = false;
    const TaskResult task =
        runGuardedTask(QStringLiteral("Saving %1").arg(QFileInfo(path).fileName()),
                       TaskAccess::Snapshot, [&shot, &path, &saveErr, &wrote] {
                           wrote = pe::saveDocument(*shot, path.toStdString(), &saveErr);
                       });
    // Refused as re-entrant (see runGuardedTask): the work never ran, so there is no
    // failure to report. Saying nothing is right here; the File actions are disabled while
    // a task is in flight, so reaching this is a programming error rather than a user one,
    // and a "Save failed" box would describe something that did not happen.
    if (!task.ran && !task.threw) {
        doc_->history().abandonSave(savePoint);
        return false;
    }
    if (task.threw) {
        doc_->history().abandonSave(savePoint);
        QMessageBox::warning(
            this, QStringLiteral("Save failed"),
            QStringLiteral("Writing \"%1\" stopped with an error: %2").arg(path, task.error));
        return false;
    }
    if (!wrote) {
        doc_->history().abandonSave(savePoint);
        QMessageBox::warning(this, QStringLiteral("Save failed"),
                             saveFailureReason(doc_.get(), path, saveErr));
        return false;
    }
    // Tell history WHICH state is now on disk: the one the snapshot captured, not wherever
    // the stack has reached. If the user painted while the worker wrote, the document is
    // still dirty and commitSave says so.
    doc_->history().commitSave(savePoint);
    currentPath_ = path;
    refreshTitle();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 3000);
    return true;
}

namespace {
// Build a TextModel from the dialog's chosen string + font, the foreground ink color, and the
// document-space click point. The QFont carries family / pixel size / bold / italic.
pe::TextModel modelFromDialog(const QString& text, const QFont& font, const QColor& ink,
                              pe::Point origin) {
    return pe::TextModel{
        .text = text.toStdString(),
        .fontFamily = font.family().toStdString(),
        .pixelSize = font.pixelSize() > 0 ? font.pixelSize() : 48,
        .bold = font.bold(),
        .italic = font.italic(),
        .color =
            pe::Rgba8{static_cast<std::uint8_t>(ink.red()), static_cast<std::uint8_t>(ink.green()),
                      static_cast<std::uint8_t>(ink.blue()), 255},
        .origin = origin,
    };
}
}  // namespace

void MainWindow::onAddText(const QPointF& docPos) {
    if (doc_ == nullptr) return;
    TextDialog dlg(this);
    dlg.setColor(fgColor_);  // start the ink swatch at the current foreground color
    if (dlg.exec() != QDialog::Accepted) return;
    const QString text = dlg.text();
    if (text.trimmed().isEmpty()) return;  // whitespace-only renders no ink -> no layer

    // A live, re-editable text layer: the app rasterizes (Qt fonts) into the cached raster the
    // engine composites; the editable model rides along so it can be reopened. The raster is
    // anchored top-left at the click (rasterOrigin == model.origin) so re-edits don't shift it.
    const pe::Point origin{static_cast<int>(std::lround(docPos.x())),
                           static_cast<int>(std::lround(docPos.y()))};
    const pe::TextModel model = modelFromDialog(text, dlg.font(), dlg.color(), origin);
    pe::PixelBuffer raster = rasterizeText(model);
    if (raster.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Text is too large to rasterize."), 4000);
        return;
    }
    auto layer = std::make_unique<pe::TextLayer>(model, std::move(raster), model.origin);
    const pe::LayerId id = layer->id();
    doc_->history().push(
        std::make_unique<pe::AddLayerCommand>(std::move(layer), doc_->topLevelCount()));
    doc_->setActiveLayer(id);
}

void MainWindow::editTextLayer(pe::LayerId id) {
    if (doc_ == nullptr) return;
    pe::Layer* layer = doc_->findLayer(id);
    if (layer == nullptr || layer->kind() != pe::LayerKind::Text) return;
    auto* textLayer = static_cast<pe::TextLayer*>(layer);
    const pe::TextModel current = textLayer->model();
    // Preserve the layer's ACTUAL raster placement across the edit (not just model.origin): the two
    // can differ for a layer loaded from a .pedoc, and reusing the real rasterOrigin keeps the
    // glyphs from snapping. The in-app producer keeps them equal, so this is a no-op there.
    const pe::Point rasterOrigin = textLayer->rasterOrigin();

    // Reopen the dialog seeded from the layer's current text + font + ink color.
    TextDialog dlg(this);
    QFont font(QString::fromStdString(current.fontFamily));
    font.setPixelSize(current.pixelSize);
    font.setBold(current.bold);
    font.setItalic(current.italic);
    const QColor ink(current.color.r, current.color.g, current.color.b, current.color.a);
    dlg.setInitial(QString::fromStdString(current.text), font, ink);
    if (dlg.exec() != QDialog::Accepted) return;
    const QString text = dlg.text();
    if (text.trimmed().isEmpty()) return;  // whitespace-only renders no ink

    // Keep the original origin; the dialog now edits text/family/size/bold/italic/color.
    // Re-rasterize and commit one undoable EditTextCommand.
    const pe::TextModel model = modelFromDialog(text, dlg.font(), dlg.color(), current.origin);
    pe::PixelBuffer raster = rasterizeText(model);
    if (raster.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Text is too large to rasterize."), 4000);
        return;
    }
    doc_->history().push(
        std::make_unique<pe::EditTextCommand>(id, model, std::move(raster), rasterOrigin));
}

namespace {

// A QImage over a PixelBuffer, copied so it owns its bytes.
[[nodiscard]] QImage toQImage(const pe::PixelBuffer& buf) {
    if (buf.isEmpty()) return QImage();
    const QImage view(reinterpret_cast<const uchar*>(buf.data()), buf.width(), buf.height(),
                      buf.width() * 4, QImage::Format_RGBA8888);
    return view.copy();
}

[[nodiscard]] pe::PixelBuffer toPixelBuffer(const QImage& img) {
    if (img.isNull()) return pe::PixelBuffer{};
    // Converted rather than assumed: an image off the system clipboard arrives in whatever
    // format the other application used, and reinterpreting those bytes as RGBA8888 would
    // paste swapped channels or read past the end of a shorter row.
    const QImage rgba = img.convertToFormat(QImage::Format_RGBA8888);
    if (rgba.isNull()) return pe::PixelBuffer{};
    pe::PixelBuffer out(rgba.width(), rgba.height());
    for (int y = 0; y < rgba.height(); ++y) {
        const auto* row = reinterpret_cast<const pe::Rgba8*>(rgba.constScanLine(y));
        for (int x = 0; x < rgba.width(); ++x) out.set(x, y, row[x]);
    }
    return out;
}

}  // namespace

pe::Rect MainWindow::clipboardRegion() const {
    if (doc_ == nullptr) return pe::Rect{};
    return pe::copyRegionFor(*doc_, &doc_->selection());
}

void MainWindow::copyToClipboard(bool merged) {
    const char* const action = merged ? "Edit > Copy Merged" : "Edit > Copy";
    if (refuseIf(doc_ == nullptr, "edit.copy", pe::RefusalCode::NoDocument, action,
                 QStringLiteral("Open a document first."))) {
        return;
    }
    const pe::Rect region = clipboardRegion();
    if (refuseIf(region.isEmpty(), "edit.copy", pe::RefusalCode::NoEffect, action,
                 QStringLiteral("That selection selects nothing, so there is nothing to "
                                "copy."))) {
        return;
    }

    pe::PixelBuffer pixels;
    if (merged) {
        pixels = canvas_->compositeRegion(region);
        if (refuseIf(pixels.isEmpty(), "edit.copy", pe::RefusalCode::OverSizeBudget, action,
                     QStringLiteral("Copying the merged image means flattening it, and this one "
                                    "is over the %1 megapixel limit.")
                         .arg(pe::kMaxCompositeImagePixels / 1'000'000))) {
            return;
        }
        // The composite arrives as a rectangle; the selection shapes it, exactly as it shapes a
        // single-layer copy.
        pe::applySelectionAlpha(pixels, pe::Point{region.x, region.y}, &doc_->selection());
    } else {
        pixels = pe::copyLayerRegion(*doc_, doc_->activeLayer(), region, &doc_->selection());
        if (refuseIf(pixels.isEmpty(), "edit.copy", pe::RefusalCode::LayerNotPixel, action,
                     QStringLiteral("Select a pixel layer to copy from, or use Copy Merged to "
                                    "take the flattened image."))) {
            return;
        }
    }

    QGuiApplication::clipboard()->setImage(toQImage(pixels));
    statusBar()->showMessage(
        QStringLiteral("Copied %1 x %2 pixels.").arg(pixels.width()).arg(pixels.height()), 3000);
}

void MainWindow::cutToClipboard() {
    if (doc_ == nullptr) return;
    // Copy first: if it refuses, nothing has been removed, and the refusal it already reported
    // is the right one. A cut that clears without having copied is unrecoverable in one step.
    const std::size_t refusalsBefore = refusals().size();
    copyToClipboard(false);
    if (refusals().size() != refusalsBefore) return;
    clearSelection();
}

void MainWindow::clearSelection() {
    const char* const action = "Edit > Clear";
    if (refuseIf(doc_ == nullptr, "edit.clear", pe::RefusalCode::NoDocument, action,
                 QStringLiteral("Open a document first."))) {
        return;
    }
    const pe::Rect region = clipboardRegion();
    if (refuseIf(region.isEmpty(), "edit.clear", pe::RefusalCode::NoEffect, action,
                 QStringLiteral("That selection selects nothing, so there is nothing to "
                                "clear."))) {
        return;
    }
    auto cmd = pe::clearRegion(*doc_, doc_->activeLayer(), region, &doc_->selection());
    if (refuseIf(cmd == nullptr, "edit.clear", pe::RefusalCode::LayerNotPixel, action,
                 QStringLiteral("Select a pixel layer to clear."))) {
        return;
    }
    doc_->history().push(std::move(cmd));
}

void MainWindow::pasteFromClipboard(bool into) {
    const char* const action = into ? "Edit > Paste Into" : "Edit > Paste";
    if (refuseIf(doc_ == nullptr, "edit.paste", pe::RefusalCode::NoDocument, action,
                 QStringLiteral("Open a document first."))) {
        return;
    }
    const pe::PixelBuffer pixels = toPixelBuffer(QGuiApplication::clipboard()->image());
    if (refuseIf(pixels.isEmpty(), "edit.paste", pe::RefusalCode::NoEffect, action,
                 QStringLiteral("The clipboard has no image in it."))) {
        return;
    }
    const bool haveSelection = doc_->selection().active();
    if (refuseIf(into && !haveSelection, "edit.paste", pe::RefusalCode::NoSelection, action,
                 QStringLiteral("Paste Into puts the pasted pixels inside a selection. Select "
                                "something first, or use Paste."))) {
        return;
    }

    // Centred on the selection when pasting into one, so the pixels line up with the mask made
    // from it; otherwise centred on the canvas, which is where the eye is.
    const pe::Rect over = into ? doc_->selection().tightBounds() : doc_->canvasBounds();
    const pe::Point origin{over.x + (over.width - pixels.width()) / 2,
                           over.y + (over.height - pixels.height()) / 2};
    auto layer = pe::layerFromBuffer(pixels, origin, into ? "Pasted Into" : "Pasted",
                                     into ? &doc_->selection() : nullptr);
    if (refuseIf(layer == nullptr, "edit.paste", pe::RefusalCode::OverSizeBudget, action,
                 QStringLiteral("That image is too large to paste into this document."))) {
        return;
    }
    const pe::LayerId id = layer->id();
    doc_->history().push(
        std::make_unique<pe::AddLayerCommand>(std::move(layer), doc_->topLevelCount()));
    doc_->setActiveLayer(id);
}

void MainWindow::addAdjustmentLayer(std::unique_ptr<pe::Adjustment> adj, const QString& name) {
    if (doc_ == nullptr || adj == nullptr) return;
    auto layer = std::make_unique<pe::AdjustmentLayer>(std::move(adj), name.toStdString());
    const pe::LayerId id = layer->id();
    doc_->history().push(
        std::make_unique<pe::AddLayerCommand>(std::move(layer), doc_->topLevelCount()));
    // Active, so the next thing done (Edit Adjustment, a mask, a blend mode) lands on the
    // layer that was just added rather than on whatever happened to be selected before.
    doc_->setActiveLayer(id);
}

void MainWindow::editAdjustmentLayer(pe::LayerId id) {
    if (doc_ == nullptr) return;
    pe::Layer* layer = doc_->findLayer(id);
    if (layer == nullptr || !layer->isAdjustment()) return;
    const pe::Adjustment& adj = static_cast<pe::AdjustmentLayer*>(layer)->adjustment();

    // Per-type: the dialog parameters seeded from the layer's CURRENT values, plus a builder that
    // turns slider values back into a fresh Adjustment. Slider-friendly types only; others fall
    // through to an informational message.
    using Builder = std::function<std::unique_ptr<pe::Adjustment>(const std::vector<double>&)>;
    QString title;
    std::vector<EffectDialog::Param> params;
    Builder build;
    const auto f = [](double v) { return static_cast<float>(v); };

    switch (adj.kind()) {
        case pe::AdjustmentKind::BrightnessContrast: {
            const auto& a = static_cast<const pe::BrightnessContrast&>(adj);
            title = QStringLiteral("Brightness/Contrast");
            params = {{QStringLiteral("Brightness"), -1.0, 1.0, a.brightness(), 2},
                      {QStringLiteral("Contrast"), -1.0, 1.0, a.contrast(), 2}};
            build = [f](const std::vector<double>& v) -> std::unique_ptr<pe::Adjustment> {
                return std::make_unique<pe::BrightnessContrast>(f(v[0]), f(v[1]));
            };
            break;
        }
        case pe::AdjustmentKind::Levels: {
            const auto& a = static_cast<const pe::Levels&>(adj);
            title = QStringLiteral("Levels");
            params = {{QStringLiteral("Input Black"), 0.0, 1.0, a.inputBlack(), 2},
                      {QStringLiteral("Input White"), 0.0, 1.0, a.inputWhite(), 2},
                      {QStringLiteral("Gamma"), 0.1, 9.99, a.gamma(), 2},
                      {QStringLiteral("Output Black"), 0.0, 1.0, a.outputBlack(), 2},
                      {QStringLiteral("Output White"), 0.0, 1.0, a.outputWhite(), 2}};
            build = [f](const std::vector<double>& v) -> std::unique_ptr<pe::Adjustment> {
                auto l = std::make_unique<pe::Levels>();
                l->setInputBlack(f(v[0]));
                l->setInputWhite(f(v[1]));
                l->setGamma(f(v[2]));
                l->setOutputBlack(f(v[3]));
                l->setOutputWhite(f(v[4]));
                return l;
            };
            break;
        }
        case pe::AdjustmentKind::Exposure: {
            const auto& a = static_cast<const pe::Exposure&>(adj);
            title = QStringLiteral("Exposure");
            params = {{QStringLiteral("Exposure (stops)"), -5.0, 5.0, a.stops(), 2},
                      {QStringLiteral("Offset"), -0.5, 0.5, a.offset(), 3},
                      {QStringLiteral("Gamma"), 0.1, 5.0, a.gamma(), 2}};
            build = [f](const std::vector<double>& v) -> std::unique_ptr<pe::Adjustment> {
                return std::make_unique<pe::Exposure>(f(v[0]), f(v[1]), f(v[2]));
            };
            break;
        }
        case pe::AdjustmentKind::Vibrance: {
            const auto& a = static_cast<const pe::Vibrance&>(adj);
            title = QStringLiteral("Vibrance");
            params = {{QStringLiteral("Vibrance"), -1.0, 1.0, a.vibrance(), 2},
                      {QStringLiteral("Saturation"), -1.0, 1.0, a.saturation(), 2}};
            build = [f](const std::vector<double>& v) -> std::unique_ptr<pe::Adjustment> {
                return std::make_unique<pe::Vibrance>(f(v[0]), f(v[1]));
            };
            break;
        }
        case pe::AdjustmentKind::HueSaturation: {
            const auto& a = static_cast<const pe::HueSaturation&>(adj);
            title = QStringLiteral("Hue/Saturation");
            params = {{QStringLiteral("Hue"), -180.0, 180.0, a.hueShiftDegrees(), 0},
                      {QStringLiteral("Saturation"), 0.0, 2.0, a.saturationScale(), 2},
                      {QStringLiteral("Lightness"), -1.0, 1.0, a.lightness(), 2}};
            // Seed from a clone so parameters the dialog doesn't surface (colorize) are preserved.
            std::shared_ptr<const pe::Adjustment> base = a.clone();
            build = [base, f](const std::vector<double>& v) -> std::unique_ptr<pe::Adjustment> {
                auto out = base->clone();
                auto* h = static_cast<pe::HueSaturation*>(out.get());
                h->setHueShiftDegrees(f(v[0]));
                h->setSaturationScale(f(v[1]));
                h->setLightness(f(v[2]));
                return out;
            };
            break;
        }
        case pe::AdjustmentKind::ColorBalance: {
            const auto& a = static_cast<const pe::ColorBalance&>(adj);
            title = QStringLiteral("Color Balance (Midtones)");
            params = {{QStringLiteral("Cyan / Red"), -1.0, 1.0, a.midtone(0), 2},
                      {QStringLiteral("Magenta / Green"), -1.0, 1.0, a.midtone(1), 2},
                      {QStringLiteral("Yellow / Blue"), -1.0, 1.0, a.midtone(2), 2}};
            // Seed from a clone so the shadows/highlights ranges and preserve-luminosity (which the
            // midtones-only dialog doesn't surface) survive the edit.
            std::shared_ptr<const pe::Adjustment> base = a.clone();
            build = [base, f](const std::vector<double>& v) -> std::unique_ptr<pe::Adjustment> {
                auto out = base->clone();
                static_cast<pe::ColorBalance*>(out.get())->setMidtones(f(v[0]), f(v[1]), f(v[2]));
                return out;
            };
            break;
        }
        case pe::AdjustmentKind::BlackAndWhite: {
            const auto& a = static_cast<const pe::BlackAndWhite&>(adj);
            title = QStringLiteral("Black & White");
            // One luminance-mix weight per color band (Photoshop's six). Range matches the engine's
            // clamp ([-2, 3]); the layer's defaults already give a neutral-ish grayscale.
            using BW = pe::BlackAndWhite;
            params = {{QStringLiteral("Reds"), -2.0, 3.0, a.band(BW::Reds), 2},
                      {QStringLiteral("Yellows"), -2.0, 3.0, a.band(BW::Yellows), 2},
                      {QStringLiteral("Greens"), -2.0, 3.0, a.band(BW::Greens), 2},
                      {QStringLiteral("Cyans"), -2.0, 3.0, a.band(BW::Cyans), 2},
                      {QStringLiteral("Blues"), -2.0, 3.0, a.band(BW::Blues), 2},
                      {QStringLiteral("Magentas"), -2.0, 3.0, a.band(BW::Magentas), 2}};
            build = [f](const std::vector<double>& v) -> std::unique_ptr<pe::Adjustment> {
                auto bw = std::make_unique<pe::BlackAndWhite>();
                for (int b = 0; b < pe::BlackAndWhite::kBandCount; ++b) {
                    bw->setBand(b, f(v[static_cast<std::size_t>(b)]));
                }
                return bw;
            };
            break;
        }
        case pe::AdjustmentKind::Posterize: {
            const auto& a = static_cast<const pe::Posterize&>(adj);
            title = QStringLiteral("Posterize");
            params = {{QStringLiteral("Levels"), 2.0, 255.0, static_cast<double>(a.levels()), 0}};
            build = [](const std::vector<double>& v) -> std::unique_ptr<pe::Adjustment> {
                return std::make_unique<pe::Posterize>(static_cast<int>(std::lround(v[0])));
            };
            break;
        }
        case pe::AdjustmentKind::Threshold: {
            const auto& a = static_cast<const pe::Threshold&>(adj);
            title = QStringLiteral("Threshold");
            params = {{QStringLiteral("Level"), 0.0, 1.0, a.level(), 2}};
            build = [f](const std::vector<double>& v) -> std::unique_ptr<pe::Adjustment> {
                return std::make_unique<pe::Threshold>(f(v[0]));
            };
            break;
        }
        case pe::AdjustmentKind::PhotoFilter: {
            const auto& a = static_cast<const pe::PhotoFilter&>(adj);
            const pe::Rgbaf col = a.color();
            title = QStringLiteral("Photo Filter");
            // Values: [r, g, b, density, preserveLuminosity].
            params = {{.label = QStringLiteral("Filter Color"),
                       .kind = EffectDialog::Param::Color,
                       .r = col.r,
                       .g = col.g,
                       .b = col.b},
                      {QStringLiteral("Density"), 0.0, 1.0, a.density(), 2},
                      {.label = QStringLiteral("Preserve Luminosity"),
                       .value = a.preserveLuminosity() ? 1.0 : 0.0,
                       .kind = EffectDialog::Param::Check}};
            build = [f](const std::vector<double>& v) -> std::unique_ptr<pe::Adjustment> {
                auto pf = std::make_unique<pe::PhotoFilter>(
                    pe::Rgbaf{f(v[0]), f(v[1]), f(v[2]), 1.0f}, f(v[3]));
                pf->setPreserveLuminosity(v[4] > 0.5);
                return pf;
            };
            break;
        }
        case pe::AdjustmentKind::GradientMap: {
            const auto& a = static_cast<const pe::GradientMap&>(adj);
            const pe::Rgbaf c0 = a.color0();
            const pe::Rgbaf c1 = a.color1();
            title = QStringLiteral("Gradient Map");
            // Values: [r0, g0, b0, r1, g1, b1, reverse].
            params = {{.label = QStringLiteral("Start (shadows)"),
                       .kind = EffectDialog::Param::Color,
                       .r = c0.r,
                       .g = c0.g,
                       .b = c0.b},
                      {.label = QStringLiteral("End (highlights)"),
                       .kind = EffectDialog::Param::Color,
                       .r = c1.r,
                       .g = c1.g,
                       .b = c1.b},
                      {.label = QStringLiteral("Reverse"),
                       .value = a.reverse() ? 1.0 : 0.0,
                       .kind = EffectDialog::Param::Check}};
            build = [f](const std::vector<double>& v) -> std::unique_ptr<pe::Adjustment> {
                auto gm =
                    std::make_unique<pe::GradientMap>(pe::Rgbaf{f(v[0]), f(v[1]), f(v[2]), 1.0f},
                                                      pe::Rgbaf{f(v[3]), f(v[4]), f(v[5]), 1.0f});
                gm->setReverse(v[6] > 0.5);
                return gm;
            };
            break;
        }
        case pe::AdjustmentKind::Curves: {
            // Curves needs an interactive curve plot, not sliders, so it uses its own dialog (with
            // the same live-preview + single-undo-step flow as EffectDialog) and returns here.
            const auto& a = static_cast<const pe::Curves&>(adj);
            CurvesDialog dlg(
                this, a.points(),
                [id](const std::vector<std::pair<float, float>>& pts)
                    -> std::unique_ptr<pe::Command> {
                    auto c = std::make_unique<pe::Curves>();
                    c->setPoints(pts);
                    return std::make_unique<pe::EditAdjustmentCommand>(id, std::move(c));
                },
                doc_.get(), [this] { canvas_->reloadImage(); });
            connect(&dlg, &CurvesDialog::refused, this, &MainWindow::reportRefusal);
            dlg.exec();
            return;
        }
        case pe::AdjustmentKind::ChannelMixer: {
            // A group per output channel (R/G/B), four sliders per group (source R/G/B + constant),
            // and a global Monochrome flag. Its own combo-driven dialog (returns here).
            const auto& a = static_cast<const pe::ChannelMixer&>(adj);
            std::vector<std::vector<double>> init(3, std::vector<double>(4));
            for (int out = 0; out < 3; ++out) {
                for (int in = 0; in < 4; ++in) init[out][in] = a.coeff(out, in);
            }
            GroupedSlidersDialog dlg(
                this, QStringLiteral("Channel Mixer"),
                {QStringLiteral("Red"), QStringLiteral("Green"), QStringLiteral("Blue")},
                {{QStringLiteral("Red"), -2.0, 2.0, 2},
                 {QStringLiteral("Green"), -2.0, 2.0, 2},
                 {QStringLiteral("Blue"), -2.0, 2.0, 2},
                 {QStringLiteral("Constant"), -1.0, 1.0, 2}},
                std::move(init), QStringLiteral("Monochrome"), a.monochrome(),
                [id, f](const std::vector<std::vector<double>>& g,
                        bool mono) -> std::unique_ptr<pe::Command> {
                    auto cm = std::make_unique<pe::ChannelMixer>();
                    for (int out = 0; out < 3; ++out) {
                        cm->setRow(out, f(g[out][0]), f(g[out][1]), f(g[out][2]), f(g[out][3]));
                    }
                    cm->setMonochrome(mono);
                    return std::make_unique<pe::EditAdjustmentCommand>(id, std::move(cm));
                },
                doc_.get(), [this] { canvas_->reloadImage(); });
            connect(&dlg, &GroupedSlidersDialog::refused, this, &MainWindow::reportRefusal);
            dlg.exec();
            return;
        }
        case pe::AdjustmentKind::SelectiveColor: {
            // A group per color range, four C/M/Y/K sliders per group, and a Relative/Absolute
            // flag.
            using SC = pe::SelectiveColor;
            const auto& a = static_cast<const SC&>(adj);
            std::vector<std::vector<double>> init(SC::kRangeCount, std::vector<double>(4));
            for (int r = 0; r < SC::kRangeCount; ++r) {
                const SC::Cmyk v = a.range(r);
                init[static_cast<std::size_t>(r)] = {v.c, v.m, v.y, v.k};
            }
            GroupedSlidersDialog dlg(
                this, QStringLiteral("Selective Color"),
                {QStringLiteral("Reds"), QStringLiteral("Yellows"), QStringLiteral("Greens"),
                 QStringLiteral("Cyans"), QStringLiteral("Blues"), QStringLiteral("Magentas"),
                 QStringLiteral("Whites"), QStringLiteral("Neutrals"), QStringLiteral("Blacks")},
                {{QStringLiteral("Cyan"), -1.0, 1.0, 2},
                 {QStringLiteral("Magenta"), -1.0, 1.0, 2},
                 {QStringLiteral("Yellow"), -1.0, 1.0, 2},
                 {QStringLiteral("Black"), -1.0, 1.0, 2}},
                std::move(init), QStringLiteral("Relative"), a.relative(),
                [id, f](const std::vector<std::vector<double>>& g,
                        bool relative) -> std::unique_ptr<pe::Command> {
                    auto sc = std::make_unique<SC>();
                    for (int r = 0; r < SC::kRangeCount; ++r) {
                        const auto& q = g[static_cast<std::size_t>(r)];
                        sc->setRange(r, f(q[0]), f(q[1]), f(q[2]), f(q[3]));
                    }
                    sc->setRelative(relative);
                    return std::make_unique<pe::EditAdjustmentCommand>(id, std::move(sc));
                },
                doc_.get(), [this] { canvas_->reloadImage(); });
            connect(&dlg, &GroupedSlidersDialog::refused, this, &MainWindow::reportRefusal);
            dlg.exec();
            return;
        }
        default:
            QMessageBox::information(
                this, QStringLiteral("Edit Adjustment"),
                noAdjustmentEditorReason(adj.kind(), QString::fromStdString(layer->name())));
            return;
    }

    // Drive the shared live-preview dialog: each value change swaps a fresh Adjustment onto this
    // layer via an EditAdjustmentCommand (its execute==undo==swap pairs cleanly with the dialog's
    // revert-then-reapply flow), and OK commits exactly one undoable EditAdjustmentCommand.
    EffectDialog dlg(
        this, title, std::move(params),
        [id, build](const std::vector<double>& v) -> std::unique_ptr<pe::Command> {
            return std::make_unique<pe::EditAdjustmentCommand>(id, build(v));
        },
        doc_.get(), [this] { canvas_->reloadImage(); });
    dlg.exec();
}

void MainWindow::exportDocumentAs() {
    if (doc_ == nullptr) return;

    // Default the dialog to the current file's format when it is a raster format, else PNG.
    pe::ImageFormat initial = pe::formatFromExtension(currentPath_.toStdString());
    if (initial == pe::ImageFormat::Unknown || initial == pe::ImageFormat::Native) {
        initial = pe::ImageFormat::Png;
    }

    ExportDialog dlg(this, *doc_, initial);
    if (dlg.exec() != QDialog::Accepted) return;
    const pe::ImageFormat fmt = dlg.selectedFormat();
    if (fmt == pe::ImageFormat::Unknown) return;  // no raster codec available (defensive)

    // Per-format save-dialog filter and canonical extension.
    QString filter;
    QString ext;
    switch (fmt) {
        case pe::ImageFormat::Png:
            filter = QStringLiteral("PNG (*.png)");
            ext = QStringLiteral("png");
            break;
        case pe::ImageFormat::Jpeg:
            filter = QStringLiteral("JPEG (*.jpg *.jpeg)");
            ext = QStringLiteral("jpg");
            break;
        case pe::ImageFormat::Tiff:
            filter = QStringLiteral("TIFF (*.tif *.tiff)");
            ext = QStringLiteral("tif");
            break;
        case pe::ImageFormat::WebP:
            filter = QStringLiteral("WebP (*.webp)");
            ext = QStringLiteral("webp");
            break;
        default:
            return;
    }

    QString path =
        QFileDialog::getSaveFileName(this, QStringLiteral("Export As"), QString(), filter);
    if (path.isEmpty()) return;
    // Guarantee the chosen format's extension so saveDocument writes that exact format
    // (the dialog, not the typed name, is the source of truth for the format).
    if (pe::formatFromExtension(path.toStdString()) != fmt) {
        if (path.endsWith(QLatin1Char('.'))) path.chop(1);  // avoid "name..ext"
        path += QStringLiteral(".%1").arg(ext);
    }

    // From a snapshot, for the same reason as Save: an export flattens and re-encodes the
    // whole canvas, which is seconds of work on any document worth exporting, and the
    // compositor it goes through reads only layer state the snapshot owns.
    const std::unique_ptr<const pe::Document> shot = doc_->snapshot();
    if (shot == nullptr) return;
    bool wrote = false;
    pe::SaveError saveErr = pe::SaveError::None;
    const pe::ExportOptions opts = dlg.options();
    const TaskResult task =
        runGuardedTask(QStringLiteral("Exporting %1").arg(QFileInfo(path).fileName()),
                       TaskAccess::Snapshot, [&shot, &path, &opts, &wrote, &saveErr] {
                           wrote = pe::saveDocument(*shot, path.toStdString(), opts, &saveErr);
                       });
    // Refused as re-entrant (see runGuardedTask): the work never ran, so there is no
    // failure to report. Saying nothing is right here; the File actions are disabled while
    // a task is in flight, so reaching this is a programming error rather than a user one,
    // and a "Export failed" box would describe something that did not happen.
    if (!task.ran && !task.threw) return;
    if (task.threw) {
        QMessageBox::warning(
            this, QStringLiteral("Export failed"),
            QStringLiteral("Exporting \"%1\" stopped with an error: %2").arg(path, task.error));
        return;
    }
    if (!wrote) {
        // The same explanation Save As gives. This used to discard the SaveError and say
        // "Could not export", which sent the user to check disk permissions when the real
        // answer was that the canvas is over the flatten limit, or too wide for the format.
        // Export is the path someone takes to make a PNG, so it is the path that most needs
        // to say why.
        QMessageBox::warning(this, QStringLiteral("Export failed"),
                             saveFailureReason(doc_.get(), path, saveErr));
        return;
    }
    // An export is a flattened copy: unlike Save/Save As it does not change the document's
    // identity (currentPath_) or clear its dirty flag — matching Photoshop/GIMP semantics.
    statusBar()->showMessage(QStringLiteral("Exported %1").arg(path), 3000);
}

void MainWindow::undo() {
    if (doc_ == nullptr) return;
    if (refuseIf(canvas_ != nullptr && canvas_->tool().isStroking(), "edit.undo",
                 pe::RefusalCode::StrokeInProgress, "Edit > Undo",
                 QStringLiteral("Finish the stroke before undoing.")) ||
        refuseIf(canvas_ != nullptr && canvas_->isTransforming(), "edit.undo",
                 pe::RefusalCode::TransformInProgress, "Edit > Undo",
                 QStringLiteral("Press Enter to apply the transform, or Esc to cancel it, "
                                "before undoing."))) {
        return;
    }
    // A live brush stroke OR Free Transform applies an uncommitted preview straight to the tiles
    // (outside history); mutating history underneath it would desync the preview's whole-tile
    // snapshots (History::undo mutates the tiles, then the canvas reverts a now-stale preview over
    // them). Ignore undo/redo until the stroke/transform is committed (Esc cancels a transform).
    doc_->history().undo();  // notifies -> canvas refreshes
}

void MainWindow::redo() {
    if (doc_ == nullptr) return;
    if (refuseIf(canvas_ != nullptr && canvas_->tool().isStroking(), "edit.redo",
                 pe::RefusalCode::StrokeInProgress, "Edit > Redo",
                 QStringLiteral("Finish the stroke before redoing.")) ||
        refuseIf(canvas_ != nullptr && canvas_->isTransforming(), "edit.redo",
                 pe::RefusalCode::TransformInProgress, "Edit > Redo",
                 QStringLiteral("Press Enter to apply the transform, or Esc to cancel it, "
                                "before redoing."))) {
        return;
    }
    doc_->history().redo();  // notifies -> canvas refreshes
}

void MainWindow::setDocument(std::unique_ptr<pe::Document> doc, QString path) {
    // Detach the observing widgets from the outgoing document before it is destroyed.
    canvas_->setDocument(nullptr);
    if (layers_ != nullptr) layers_->setDocument(nullptr);
    if (history_ != nullptr) history_->setDocument(nullptr);
    if (properties_ != nullptr) properties_->setDocument(nullptr);
    if (channels_ != nullptr) channels_->setDocument(nullptr);
    if (doc_ != nullptr) doc_->removeObserver(this);
    doc_ = std::move(doc);
    if (doc_ != nullptr) doc_->addObserver(this);
    currentPath_ = std::move(path);
    canvas_->setDocument(doc_.get());
    if (layers_ != nullptr) layers_->setDocument(doc_.get());
    if (history_ != nullptr) history_->setDocument(doc_.get());
    if (properties_ != nullptr) properties_->setDocument(doc_.get());
    if (channels_ != nullptr) channels_->setDocument(doc_.get());
    refreshTitle();
    refreshDocTab();
    refreshZoomStrip();
    updateActionStates();
}

bool resolveUnsavedChanges(const std::function<bool()>& isDirty,
                           const std::function<DiscardAnswer()>& ask,
                           const std::function<bool()>& save) {
    // Re-test every pass, rather than trusting the outcome of the save. See the header.
    while (isDirty()) {
        switch (ask()) {
            case DiscardAnswer::Save:
                // Only continue if the write actually succeeded; a failed or cancelled
                // Save As must not fall through to discarding the document.
                if (!save()) return false;
                break;  // round again: anything painted during that write is still unsaved
            case DiscardAnswer::Discard:
                return true;
            case DiscardAnswer::Cancel:
            default:
                return false;
        }
    }
    // Nothing to lose: no document, or every change is already on disk.
    return true;
}

DiscardAnswer MainWindow::askAboutUnsavedChanges() {
    const QString name =
        currentPath_.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(currentPath_).fileName();
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(QStringLiteral("Unsaved changes"));
    box.setText(QStringLiteral("Save changes to \"%1\" before closing?").arg(name));
    box.setInformativeText(QStringLiteral("If you don't save, your changes will be lost."));
    box.setStandardButtons(QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Save);

    switch (box.exec()) {
        case QMessageBox::Save:
            return DiscardAnswer::Save;
        case QMessageBox::Discard:
            return DiscardAnswer::Discard;
        default:
            return DiscardAnswer::Cancel;  // Cancel, or the dialog was closed
    }
}

bool MainWindow::confirmDiscard() {
    return resolveUnsavedChanges([this] { return doc_ != nullptr && doc_->isDirty(); },
                                 [this] { return askAboutUnsavedChanges(); },
                                 [this] { return saveDocument(); });
}

void MainWindow::closeEvent(QCloseEvent* e) {
    // A background task holds a snapshot, and the nested event loop it is running inside
    // sits on this window's stack. Closing now would destroy both. Refuse rather than
    // defer: the task finishes in seconds and the user can close then. BusyTask's input
    // filter refuses the event before it reaches here, so this is the second line of
    // defence, and the one a reader looking for the rule will find.
    if (documentTaskInFlight_) {
        e->ignore();
        return;
    }
    if (confirmDiscard()) {
        e->accept();
    } else {
        e->ignore();
    }
}

void MainWindow::onDocumentChanged(const pe::Document& doc, const pe::DocumentChange& change) {
    // Only the modified marker depends on this; the panels observe for their own data.
    if (&doc != doc_.get()) return;
    if (change.kind == pe::DocumentChange::Kind::DirtyState) refreshTitle();
    // Any committed change can move the undo/redo boundaries.
    updateActionStates();
}

void MainWindow::refreshTitle() {
    const QString name =
        currentPath_.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(currentPath_).fileName();
    // Leading marker for unsaved changes, matching the platform convention.
    const QString shown = (doc_ != nullptr && doc_->isDirty()) ? QStringLiteral("*") + name : name;
    setWindowTitle(QStringLiteral("%1 — PhotoEdit %2")
                       .arg(doc_ ? shown : QStringLiteral("(no document)"))
                       .arg(pe::Version::string()));
}

void MainWindow::buildDockPanels() {
    // Photoshop-style tabbed groups: the QTabBar is the header, so each dock hides
    // its native title bar. Three groups stack vertically on the right.
    setTabPosition(Qt::RightDockWidgetArea, QTabWidget::North);

    auto makeDock = [this](const QString& title, QWidget* content) {
        auto* dock = new QDockWidget(title, this);
        dock->setObjectName(title);
        // Closable so the Window menu toggles actually do something. The native title
        // bar stays hidden (the tab is the header), so the Window menu is the way back.
        dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable |
                          QDockWidget::DockWidgetClosable);
        dock->setTitleBarWidget(new QWidget(dock));  // hide native title; tabs are the header
        dock->setWidget(content);
        return dock;
    };
    // A panel that is not built yet says so, and says what it will be for. It used to show
    // its own name centred in an otherwise blank dock, which is indistinguishable from a
    // panel that is broken: the user reported these as "empty". This is the same honest
    // treatment the scaffolded tools already get in their tooltips.
    auto placeholder = [](const QString& name, const QString& purpose) {
        auto* w = new QWidget();
        w->setObjectName(QStringLiteral("PanelPlaceholder"));
        w->setAccessibleName(name);
        w->setAccessibleDescription(QStringLiteral("Not yet implemented"));
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(16, 16, 16, 16);
        v->setSpacing(6);
        v->addStretch(1);
        auto* title = new QLabel(name, w);
        title->setObjectName(QStringLiteral("PlaceholderTitle"));
        title->setAlignment(Qt::AlignCenter);
        auto* body = new QLabel(purpose, w);
        body->setObjectName(QStringLiteral("PlaceholderBody"));
        body->setAlignment(Qt::AlignCenter);
        body->setWordWrap(true);
        auto* note = new QLabel(QStringLiteral("Not yet implemented"), w);
        note->setObjectName(QStringLiteral("PlaceholderNote"));
        note->setAlignment(Qt::AlignCenter);
        v->addWidget(title);
        v->addWidget(body);
        v->addWidget(note);
        v->addStretch(1);
        return static_cast<QWidget*>(w);
    };

    layers_ = new LayersPanel();
    // Deleting a layer that holds pixels is not undoable-by-accident enough to do silently.
    // The panel asks through this hook rather than owning a dialog, so the rule stays testable
    // and the modal lives in one place, as the unsaved-changes prompt does.
    layers_->setDeleteConfirmer([this](const QString& name) {
        return QMessageBox::question(
                   this, QStringLiteral("Delete layer"),
                   QStringLiteral("Delete \"%1\"? It still has content.").arg(name),
                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
    });
    connect(layers_, &LayersPanel::editAdjustmentRequested, this, &MainWindow::editAdjustmentLayer);
    connect(layers_, &LayersPanel::refused, this, &MainWindow::reportRefusal);
    connect(layers_, &LayersPanel::editTextRequested, this, &MainWindow::editTextLayer);
    // Clicking a mask thumbnail targets it for brush painting; route that to the canvas so the
    // Brush paints the active layer's mask (black hides, white reveals) until the target is
    // cleared.
    connect(layers_, &LayersPanel::maskEditTargetChanged, this, [this](bool on) {
        if (canvas_ != nullptr) canvas_->setMaskEditTarget(on);
    });
    // The canvas exits mask-edit when a non-Brush tool is chosen; drop the panel's ring to match.
    connect(canvas_, &CanvasView::maskEditTargetCleared, this, [this] {
        if (layers_ != nullptr) layers_->clearMaskTarget();
    });
    history_ = new HistoryPanel();
    colorPanel_ = new ColorPanel();
    swatchesPanel_ = new SwatchesPanel();
    adjustments_ = new AdjustmentsPanel();
    channels_ = new ChannelsPanel();
    gradients_ = new GradientsPanel();
    properties_ = new PropertiesPanel();

    // The Color panel drives the foreground/brush colour; seed it and keep in sync.
    colorPanel_->setColor(fgColor_);
    connect(colorPanel_, &ColorPanel::colorChanged, this,
            [this](const QColor& c) { setForegroundColor(c); });
    // The Swatches grid is a palette, not a second editor: choosing a chip goes through the
    // same one place the picker and the eyedropper do, so the two panels, the tool strip's
    // swatch and the brush cannot drift apart.
    swatchesPanel_->setCurrentColor(fgColor_);
    connect(swatchesPanel_, &SwatchesPanel::colorChosen, this,
            [this](const QColor& c) { setForegroundColor(c); });
    // A preset is a name and a set of numbers; the layer it becomes is built here, by the
    // same call Layer▸New Adjustment Layer makes. The panel never touches the document.
    connect(adjustments_, &AdjustmentsPanel::presetChosen, this, [this](int index) {
        if (adjustments_ == nullptr) return;
        addAdjustmentLayer(adjustments_->makeAdjustment(index), adjustments_->preset(index).name);
    });
    // Channels is display state: the panel decides what the canvas draws and never touches
    // the document. Its thumbnails come from the canvas's renderer, which already holds the
    // composited tiles, rather than from a fresh flatten of the whole image.
    channels_->setPreviewSource(
        [this](int maxPixels) { return canvas_->canvasPreview(maxPixels); });
    connect(channels_, &ChannelsPanel::viewChanged, this,
            [this](pe::ChannelView v) { canvas_->setChannelView(v); });
    connect(channels_, &ChannelsPanel::loadAsSelectionRequested, this,
            [this](std::optional<pe::Channel> ch) { canvas_->loadSelectionFromChannel(ch); });
    // The chosen ramp is a tool setting, like the brush size: the panel picks it, the canvas
    // draws with it, and the two stops that follow the loaded colours are resolved on each
    // drag rather than baked in here.
    gradients_->setColors(fgColor_, bgColor_);
    connect(gradients_, &GradientsPanel::gradientChosen, this, [this](int index) {
        if (gradients_ == nullptr) return;
        canvas_->setGradient(gradients_->preset(index).gradient);
    });

    // Group anchors (one per stacked group).
    auto* colorDock = makeDock(QStringLiteral("Color"), colorPanel_);
    auto* propsDock = makeDock(QStringLiteral("Properties"), properties_);
    auto* layersDock = makeDock(QStringLiteral("Layers"), layers_);

    // Establish the three vertical regions FIRST (split before tabify, or Qt merges
    // everything into one tab group).
    addDockWidget(Qt::RightDockWidgetArea, colorDock);
    splitDockWidget(colorDock, propsDock, Qt::Vertical);
    splitDockWidget(propsDock, layersDock, Qt::Vertical);

    // Then fill each group's tabs, always tabifying onto the group's anchor so the
    // insertion order (and grouping) is preserved.
    tabifyDockWidget(colorDock, makeDock(QStringLiteral("Swatches"), swatchesPanel_));
    tabifyDockWidget(colorDock, makeDock(QStringLiteral("Gradients"), gradients_));
    tabifyDockWidget(colorDock,
                     makeDock(QStringLiteral("Patterns"),
                              placeholder(QStringLiteral("Patterns"),
                                          QStringLiteral("Tiling images to fill a selection "
                                                         "or a layer with."))));

    tabifyDockWidget(propsDock, makeDock(QStringLiteral("Adjustments"), adjustments_));
    tabifyDockWidget(
        propsDock, makeDock(QStringLiteral("Libraries"),
                            placeholder(QStringLiteral("Libraries"),
                                        QStringLiteral("Assets shared between documents: colours, "
                                                       "gradients, graphics."))));

    tabifyDockWidget(layersDock, makeDock(QStringLiteral("Channels"), channels_));
    tabifyDockWidget(layersDock,
                     makeDock(QStringLiteral("Paths"),
                              placeholder(QStringLiteral("Paths"),
                                          QStringLiteral("Vector paths from the Pen tool, and "
                                                         "selections converted to and from "
                                                         "them."))));
    tabifyDockWidget(layersDock, makeDock(QStringLiteral("History"), history_));

    colorDock->raise();  // first tab of each group is the active one
    propsDock->raise();
    layersDock->raise();
}

void MainWindow::buildStatusBar() {
    toolLabel_ = new QLabel(QStringLiteral("Brush"), this);
    toolLabel_->setObjectName(QStringLiteral("StatusToolName"));
    toolHintLabel_ = new QLabel(QStringLiteral("Drag to paint with the foreground color"), this);
    toolHintLabel_->setObjectName(QStringLiteral("StatusToolHint"));
    posLabel_ = new QLabel(this);
    posLabel_->setObjectName(QStringLiteral("StatusCursorPos"));
    zoomLabel_ = new QLabel(QStringLiteral("—"), this);

    statusBar()->addWidget(toolLabel_);
    statusBar()->addWidget(toolHintLabel_, 1);  // takes the slack so the readouts stay right
    statusBar()->addPermanentWidget(posLabel_);
    statusBar()->addPermanentWidget(zoomLabel_);
    clearCursorPos();

    // Live cursor position in document pixels, which is what a user measuring or
    // aligning actually needs; the widget position would be meaningless at zoom.
    connect(canvas_, &CanvasView::cursorMoved, this, [this](const QPointF& docPos) {
        posLabel_->setText(QStringLiteral("X %1  Y %2")
                               .arg(static_cast<int>(std::floor(docPos.x())))
                               .arg(static_cast<int>(std::floor(docPos.y()))));
    });
    connect(canvas_, &CanvasView::cursorLeft, this, &MainWindow::clearCursorPos);
    connect(canvas_, &CanvasView::zoomChanged, this, [this](double) { refreshZoomStrip(); });
    connect(canvas_, &CanvasView::zoomChanged, this, [this](double pct) {
        zoomLabel_->setText(QStringLiteral("%1%").arg(pct, 0, 'f', 0));
        refreshDocTab();
    });
}

void MainWindow::retintIcons() {
    const QColor tint = themeIconColor();
    for (const ThemedIcon& t : themedIcons_) {
        if (t.action != nullptr) t.action->setIcon(renderIconAsIcon(t.name, tint, t.size));
    }
    for (const ThemedButton& t : themedButtons_) {
        if (t.button != nullptr) t.button->setIcon(renderIconAsIcon(t.name, tint, t.size));
    }
}

void MainWindow::setTheme(ThemeId id) {
    applyTheme(*qApp, id);
    retintIcons();  // the glyphs are tinted at render time, so they need rebuilding
    if (canvas_ != nullptr) canvas_->update();  // repaint the themed pasteboard
    QSettings().setValue(QStringLiteral("theme"), static_cast<int>(id));
}

}  // namespace pe::app
