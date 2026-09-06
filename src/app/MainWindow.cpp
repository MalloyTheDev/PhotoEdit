#include "MainWindow.hpp"

#include "PixelFormatNames.hpp"

#include "CanvasView.hpp"
#include "ColorPanel.hpp"
#include "CurvesDialog.hpp"
#include "EffectDialog.hpp"
#include "ExportDialog.hpp"
#include "GroupedSlidersDialog.hpp"
#include "HistoryPanel.hpp"
#include "IconUtil.hpp"
#include "LayersPanel.hpp"
#include "PropertiesPanel.hpp"
#include "TextDialog.hpp"
#include "TextRender.hpp"
#include "pe/core/Adjustment.hpp"
#include "pe/core/AdjustmentLayer.hpp"
#include "pe/core/Brush.hpp"  // pe::PaintCommand (effect-dialog command factories)
#include "pe/core/Color.hpp"
#include "pe/core/Commands.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/Filter.hpp"
#include "pe/core/TextLayer.hpp"  // pe::TextLayer / TextModel / EditTextCommand
#include "pe/core/Version.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QColorDialog>
#include <QComboBox>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
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
#include <functional>
#include <memory>
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
    connect(canvas_, &CanvasView::colorPicked, this, &MainWindow::onColorPicked);
    connect(canvas_, &CanvasView::toolMessage, this,
            [this](const QString& msg) { statusBar()->showMessage(msg, 4000); });
    connect(canvas_, &CanvasView::textRequested, this, &MainWindow::onAddText);

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
    fileMenu->addAction(QStringLiteral("&New"), standardOr(QKeySequence::New, "Ctrl+N"), this,
                        &MainWindow::newDocument);
    fileMenu->addAction(QStringLiteral("&Open..."), standardOr(QKeySequence::Open, "Ctrl+O"), this,
                        &MainWindow::openDocument);
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
    fileMenu->addSeparator();
    // Explicit Ctrl+Q rather than QKeySequence::Quit: on Windows that standard key
    // resolves to the unusable literal "Exit" rather than a chord.
    // Routed through close() so closeEvent can guard unsaved work; connecting
    // QApplication::quit directly bypassed the guard entirely.
    fileMenu->addAction(QStringLiteral("E&xit"), QKeySequence(QStringLiteral("Ctrl+Q")), this,
                        &MainWindow::close);

    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    undoAct_ = editMenu->addAction(QStringLiteral("&Undo"), this, &MainWindow::undo);
    undoAct_->setShortcut(QKeySequence::Undo);
    redoAct_ = editMenu->addAction(QStringLiteral("&Redo"), this, &MainWindow::redo);
    redoAct_->setShortcut(QKeySequence::Redo);
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
            if (l != nullptr && l->mask() == nullptr) {  // only a not-yet-masked layer
                doc_->history().push(
                    std::make_unique<pe::AddLayerMaskCommand>(doc_->activeLayer(), init));
            }
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
            if (l != nullptr && l->mask() != nullptr) {
                doc_->history().push(std::make_unique<pe::SetMaskEnabledCommand>(
                    doc_->activeLayer(), !l->mask()->enabled()));
            }
        });
        maskMenu->addAction(QStringLiteral("Delete Mask"), this, [this] {
            if (doc_ == nullptr) return;
            const pe::Layer* l = doc_->findLayer(doc_->activeLayer());
            if (l != nullptr && l->mask() != nullptr) {
                doc_->history().push(
                    std::make_unique<pe::RemoveLayerMaskCommand>(doc_->activeLayer()));
            }
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
                if (doc_ == nullptr) return;
                auto layer = std::make_unique<pe::AdjustmentLayer>(make(), std::string(label));
                const pe::LayerId id = layer->id();
                doc_->history().push(
                    std::make_unique<pe::AddLayerCommand>(std::move(layer), doc_->topLevelCount()));
                doc_->setActiveLayer(id);
            });
        }
        // Edit the active adjustment layer's parameters (also reachable by double-clicking its
        // row).
        layerMenu->addAction(QStringLiteral("Edit Adjustment..."), this, [this] {
            if (doc_ == nullptr) return;
            const pe::Layer* a = doc_->findLayer(doc_->activeLayer());
            if (a != nullptr && a->isAdjustment()) {
                editAdjustmentLayer(doc_->activeLayer());
            } else {
                statusBar()->showMessage(QStringLiteral("Select an adjustment layer to edit."),
                                         4000);
            }
        });
    }
    auto* selMenu = menuBar()->addMenu(QStringLiteral("&Select"));
    docMenus_.push_back(selMenu);
    selMenu->addAction(QStringLiteral("Select All"), standardOr(QKeySequence::SelectAll, "Ctrl+A"),
                       this, [this]() {
                           if (doc_) {
                               Selection target;
                               target.selectAll(doc_->canvasBounds());
                               doc_->history().push(std::make_unique<SetSelectionCommand>(target));
                           }
                       });
    selMenu->addAction(QStringLiteral("Deselect"), QKeySequence(QStringLiteral("Ctrl+D")), this,
                       [this]() {
                           if (doc_) {
                               Selection target;
                               target.selectNone();
                               doc_->history().push(std::make_unique<SetSelectionCommand>(target));
                           }
                       });
    selMenu->addSeparator();
    selMenu->addAction(QStringLiteral("Invert Selection"),
                       QKeySequence(QStringLiteral("Ctrl+Shift+I")), this, [this]() {
                           if (doc_) {
                               Selection target = doc_->selection();
                               target.invert(doc_->canvasBounds());
                               doc_->history().push(std::make_unique<SetSelectionCommand>(target));
                           }
                       });
    selMenu->addSeparator();
    // Edge refinements. Each prompts for an amount, applies it to a copy of the active selection,
    // and pushes the result as one undo step — but only when it actually changed the selection, so
    // a no-op (e.g. a region over the working cap) leaves no phantom undo entry.
    const auto refineSelection = [this](auto&& apply) {
        if (doc_ == nullptr || !doc_->selection().active()) return;
        Selection target = doc_->selection();
        apply(target);
        if (!(target == doc_->selection())) {
            doc_->history().push(std::make_unique<SetSelectionCommand>(target));
        }
    };
    selMenu->addAction(QStringLiteral("Grow..."), this, [this, refineSelection]() {
        bool ok = false;
        const int px =
            QInputDialog::getInt(this, QStringLiteral("Grow Selection"),
                                 QStringLiteral("Expand by (pixels):"), 4, 1, 1000, 1, &ok);
        if (ok) refineSelection([px](Selection& s) { s.grow(px); });
    });
    selMenu->addAction(QStringLiteral("Shrink..."), this, [this, refineSelection]() {
        bool ok = false;
        const int px =
            QInputDialog::getInt(this, QStringLiteral("Shrink Selection"),
                                 QStringLiteral("Contract by (pixels):"), 4, 1, 1000, 1, &ok);
        if (ok) refineSelection([px](Selection& s) { s.shrink(px); });
    });
    selMenu->addAction(QStringLiteral("Feather..."), this, [this, refineSelection]() {
        bool ok = false;
        const double r =
            QInputDialog::getDouble(this, QStringLiteral("Feather Selection"),
                                    QStringLiteral("Radius (pixels):"), 4.0, 0.1, 250.0, 1, &ok);
        if (!ok) return;
        const auto rad = static_cast<float>(r);
        const Rect canvas = doc_->canvasBounds();
        refineSelection([rad, canvas](Selection& s) { s.feather(rad, canvas); });
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

void MainWindow::updateActionStates() {
    const bool hasDoc = doc_ != nullptr;
    for (QMenu* m : docMenus_) {
        if (m != nullptr) m->setEnabled(hasDoc);
    }
    for (QAction* a : docActions_) {
        if (a != nullptr) a->setEnabled(hasDoc);
    }
    // Undo and Redo track the history rather than merely the document, so they grey
    // out at the ends of the stack instead of silently doing nothing.
    if (undoAct_ != nullptr) undoAct_->setEnabled(hasDoc && doc_->history().canUndo());
    if (redoAct_ != nullptr) redoAct_->setEnabled(hasDoc && doc_->history().canRedo());
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
    sizeSpin_->setRange(1, 500);
    sizeSpin_->setValue(static_cast<int>(canvas_->tool().brush().diameter));
    sizeSpin_->setSuffix(QStringLiteral(" px"));
    bl->addWidget(sizeSpin_);
    bl->addWidget(new QLabel(QStringLiteral("Opacity"), brushOptions_));
    opacitySpinOpt_ = new QSpinBox(brushOptions_);
    opacitySpinOpt_->setRange(1, 100);
    opacitySpinOpt_->setValue(static_cast<int>(canvas_->tool().brush().opacity * 100.0f));
    opacitySpinOpt_->setSuffix(QStringLiteral("%"));
    bl->addWidget(opacitySpinOpt_);
    bl->addWidget(new QLabel(QStringLiteral("Flow"), brushOptions_));
    auto* flowSpin = new QSpinBox(brushOptions_);
    flowSpin->setRange(1, 100);
    flowSpin->setValue(static_cast<int>(canvas_->tool().brush().flow * 100.0f));
    flowSpin->setSuffix(QStringLiteral("%"));
    bl->addWidget(flowSpin);

    // Brush dynamics UI skeleton: stabilization (0-100%)
    bl->addWidget(new QLabel(QStringLiteral("Stabilize"), brushOptions_));
    auto* stabSpin = new QSpinBox(brushOptions_);
    stabSpin->setRange(0, 100);
    stabSpin->setValue(static_cast<int>(canvas_->tool().brush().stabilize * 100.0f));
    stabSpin->setSuffix(QStringLiteral("%"));
    bl->addWidget(stabSpin);
    brushOptAction_ = optionsBar_->addWidget(brushOptions_);

    connect(sizeSpin_, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->tool().brush().diameter = static_cast<float>(v); });
    connect(opacitySpinOpt_, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->tool().brush().opacity = static_cast<float>(v) / 100.0f; });
    connect(flowSpin, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->tool().brush().flow = static_cast<float>(v) / 100.0f; });
    connect(stabSpin, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->tool().brush().stabilize = static_cast<float>(v) / 100.0f; });

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
    wandOptAction_->setVisible(false);

    connect(wandTolSpin_, &QSpinBox::valueChanged, this,
            [this](int v) { canvas_->setWandTolerance(v); });

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

void MainWindow::updateOptionsBar(OptKind kind, const QString& toolName) {
    if (optToolName_ != nullptr) optToolName_->setText(toolName);
    // Toggle the toolbar ACTIONS, not the inner widgets — QToolBar lays widgets out
    // via their wrapping action, so hiding the widget alone leaves a gap/ghost.
    if (brushOptAction_ != nullptr) brushOptAction_->setVisible(kind == OptKind::Brush);
    if (wandOptAction_ != nullptr) wandOptAction_->setVisible(kind == OptKind::Wand);
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
}

void MainWindow::onColorPicked(const QColor& c) {
    if (!c.isValid()) return;
    fgColor_ = c;
    // Mirror chooseForegroundColor: the eyedropper sets the actual paint color and
    // syncs the picker, not just the swatch.
    canvas_->tool().setColor(pe::Rgbaf{static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
                                       static_cast<float>(c.blueF()), 1.0f});
    if (colorPanel_ != nullptr) colorPanel_->setColor(fgColor_);
    updateSwatches();
    statusBar()->showMessage(QStringLiteral("Picked %1").arg(c.name()), 2000);
}

void MainWindow::chooseForegroundColor() {
    const QColor c = QColorDialog::getColor(fgColor_, this, QStringLiteral("Foreground Color"));
    if (!c.isValid()) return;
    fgColor_ = c;
    canvas_->tool().setColor(pe::Rgbaf{static_cast<float>(c.redF()), static_cast<float>(c.greenF()),
                                       static_cast<float>(c.blueF()), 1.0f});
    if (colorPanel_ != nullptr) colorPanel_->setColor(fgColor_);  // keep the picker in sync
    updateSwatches();
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

    auto doc = pe::loadDocument(path.toStdString());
    if (doc == nullptr) {
        QMessageBox::warning(this, QStringLiteral("Open failed"),
                             QStringLiteral("Could not open \"%1\".").arg(path));
        return;
    }
    // A freshly loaded document is at a "saved" state for dirty tracking.
    doc->history().markSaved();
    setDocument(std::move(doc), path);
    statusBar()->showMessage(QStringLiteral("Opened %1").arg(path), 3000);
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
    if (!pe::saveDocument(*doc_, path.toStdString())) {
        QMessageBox::warning(this, QStringLiteral("Save failed"),
                             QStringLiteral("Could not save \"%1\".").arg(path));
        return false;
    }
    // Tell history the on-disk state now matches; this clears the dirty flag
    // so isDirty()/title indicators and undo-to-saved work correctly.
    doc_->history().markSaved();
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
            dlg.exec();
            return;
        }
        default:
            QMessageBox::information(this, QStringLiteral("Edit Adjustment"),
                                     QStringLiteral("Editing “%1” isn't supported yet.")
                                         .arg(QString::fromStdString(adj.name())));
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

    if (!pe::saveDocument(*doc_, path.toStdString(), dlg.options())) {
        QMessageBox::warning(this, QStringLiteral("Export failed"),
                             QStringLiteral("Could not export \"%1\".").arg(path));
        return;
    }
    // An export is a flattened copy: unlike Save/Save As it does not change the document's
    // identity (currentPath_) or clear its dirty flag — matching Photoshop/GIMP semantics.
    statusBar()->showMessage(QStringLiteral("Exported %1").arg(path), 3000);
}

void MainWindow::undo() {
    if (doc_ == nullptr) return;
    // A live brush stroke OR Free Transform applies an uncommitted preview straight to the tiles
    // (outside history); mutating history underneath it would desync the preview's whole-tile
    // snapshots (History::undo mutates the tiles, then the canvas reverts a now-stale preview over
    // them). Ignore undo/redo until the stroke/transform is committed (Esc cancels a transform).
    if (canvas_ != nullptr && (canvas_->tool().isStroking() || canvas_->isTransforming())) return;
    doc_->history().undo();  // notifies -> canvas refreshes
}

void MainWindow::redo() {
    if (doc_ == nullptr) return;
    if (canvas_ != nullptr && (canvas_->tool().isStroking() || canvas_->isTransforming())) return;
    doc_->history().redo();  // notifies -> canvas refreshes
}

void MainWindow::setDocument(std::unique_ptr<pe::Document> doc, QString path) {
    // Detach the observing widgets from the outgoing document before it is destroyed.
    canvas_->setDocument(nullptr);
    if (layers_ != nullptr) layers_->setDocument(nullptr);
    if (history_ != nullptr) history_->setDocument(nullptr);
    if (properties_ != nullptr) properties_->setDocument(nullptr);
    if (doc_ != nullptr) doc_->removeObserver(this);
    doc_ = std::move(doc);
    if (doc_ != nullptr) doc_->addObserver(this);
    currentPath_ = std::move(path);
    canvas_->setDocument(doc_.get());
    if (layers_ != nullptr) layers_->setDocument(doc_.get());
    if (history_ != nullptr) history_->setDocument(doc_.get());
    if (properties_ != nullptr) properties_->setDocument(doc_.get());
    refreshTitle();
    refreshDocTab();
    refreshZoomStrip();
    updateActionStates();
}

bool MainWindow::confirmDiscard() {
    // Nothing to lose: no document, or every change is already on disk.
    if (doc_ == nullptr || !doc_->isDirty()) return true;

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
            // Only proceed if the write actually succeeded; a failed or cancelled
            // Save As must not fall through to discarding the document.
            return saveDocument();
        case QMessageBox::Discard:
            return true;
        default:
            return false;  // Cancel, or the dialog was closed
    }
}

void MainWindow::closeEvent(QCloseEvent* e) {
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
    auto placeholder = [](const QString& name) {
        auto* l = new QLabel(name);
        l->setObjectName(QStringLiteral("PanelPlaceholder"));
        l->setAlignment(Qt::AlignCenter);
        return static_cast<QWidget*>(l);
    };

    layers_ = new LayersPanel();
    connect(layers_, &LayersPanel::editAdjustmentRequested, this, &MainWindow::editAdjustmentLayer);
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
    properties_ = new PropertiesPanel();

    // The Color panel drives the foreground/brush colour; seed it and keep in sync.
    colorPanel_->setColor(fgColor_);
    connect(colorPanel_, &ColorPanel::colorChanged, this, [this](const QColor& c) {
        fgColor_ = c;
        canvas_->tool().setColor(pe::Rgbaf{static_cast<float>(c.redF()),
                                           static_cast<float>(c.greenF()),
                                           static_cast<float>(c.blueF()), 1.0f});
        updateSwatches();
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
    tabifyDockWidget(colorDock,
                     makeDock(QStringLiteral("Swatches"), placeholder(QStringLiteral("Swatches"))));
    tabifyDockWidget(
        colorDock, makeDock(QStringLiteral("Gradients"), placeholder(QStringLiteral("Gradients"))));
    tabifyDockWidget(colorDock,
                     makeDock(QStringLiteral("Patterns"), placeholder(QStringLiteral("Patterns"))));

    tabifyDockWidget(propsDock, makeDock(QStringLiteral("Adjustments"),
                                         placeholder(QStringLiteral("Adjustments"))));
    tabifyDockWidget(
        propsDock, makeDock(QStringLiteral("Libraries"), placeholder(QStringLiteral("Libraries"))));

    tabifyDockWidget(layersDock,
                     makeDock(QStringLiteral("Channels"), placeholder(QStringLiteral("Channels"))));
    tabifyDockWidget(layersDock,
                     makeDock(QStringLiteral("Paths"), placeholder(QStringLiteral("Paths"))));
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
