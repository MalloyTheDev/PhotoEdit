#pragma once

#include "Theme.hpp"

#include "pe/core/Document.hpp"  // pe::DocumentObserver (base class)
#include "pe/core/Layer.hpp"     // pe::LayerId

#include <QMainWindow>
#include <QString>

#include <memory>

class QAction;
class QCloseEvent;
class QLabel;
class QMenu;
class QPointF;
class QSpinBox;
class QToolBar;
class QToolButton;
class QWidget;

namespace pe::app {

class CanvasView;
class LayersPanel;
class HistoryPanel;
class ColorPanel;
class PropertiesPanel;

// The top-level application window. Wires the menus to the engine's document I/O and
// shows the active document on a CanvasView. Color, Properties, Layers and History are
// real panels; Swatches, Gradients, Patterns, Adjustments, Libraries, Channels and
// Paths are still placeholders. See docs/systems/24-ui-workspace.md.
class MainWindow : public QMainWindow, public pe::DocumentObserver {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    // Guards the window close against unsaved work. File > Exit routes through
    // close() so this runs for both paths.
    void closeEvent(QCloseEvent* e) override;

    // The title carries the modified marker, so it has to track the dirty bit rather
    // than only being refreshed when the document is swapped.
    void onDocumentChanged(const pe::Document& doc, const pe::DocumentChange& change) override;

private:
    // Offers Save / Discard / Cancel when the current document has unsaved changes.
    // Returns false only if the user cancels, in which case the caller must abort.
    // True when there is nothing to lose, when the user saved, or when they discarded.
    [[nodiscard]] bool confirmDiscard();

    void buildMenuBar();
    void buildToolBar();
    void buildOptionsBar();
    void buildCentral();
    void buildDockPanels();
    void buildStatusBar();
    void setTheme(ThemeId id);
    // Fills the Window menu with one toggle per dock. Must run after buildDockPanels,
    // since the toggles come from the docks themselves.
    void populateWindowMenu();
    void showAbout();
    // Canvas dimensions and the zoom readout in the options bar. Driven by the
    // document swap and by CanvasView::zoomChanged.
    void refreshZoomStrip();
    void clearCursorPos();  // blanks the position readout when the cursor is off-canvas

    // Which contextual control group the options bar shows for the active tool.
    enum class OptKind { None, Brush, Wand };
    void updateOptionsBar(OptKind kind, const QString& toolName);
    void refreshDocTab();
    [[nodiscard]] QWidget* makeColorSwatches();
    void chooseForegroundColor();
    void chooseBackgroundColor();
    void updateSwatches();

    void newDocument();
    void openDocument();
    bool saveDocument();      // saves to the current path, or prompts if none
    bool saveDocumentAs();    // always prompts
    void exportDocumentAs();  // flatten + encode to a raster format with per-format options
    void undo();
    void redo();
    void onAddText(const QPointF& docPos);  // Type tool: prompt + rasterize + stamp text
    // Open the live-preview parameter dialog for an adjustment layer (double-click in the Layers
    // panel, or Layer▸Edit Adjustment). Commits one EditAdjustmentCommand on OK; a no-op for a
    // non-adjustment layer or a type without an editor yet.
    void editAdjustmentLayer(pe::LayerId id);
    // Reopen the text dialog for a text layer (double-click in the Layers panel), seeded from its
    // model; commits one EditTextCommand on OK. A no-op for a non-text layer.
    void editTextLayer(pe::LayerId id);
    bool writeTo(const QString& path);
    void setDocument(std::unique_ptr<pe::Document> doc, QString path);
    void refreshTitle();
    void onColorPicked(const QColor& c);

    std::unique_ptr<pe::Document> doc_;
    CanvasView* canvas_ = nullptr;
    LayersPanel* layers_ = nullptr;
    HistoryPanel* history_ = nullptr;
    ColorPanel* colorPanel_ = nullptr;
    PropertiesPanel* properties_ = nullptr;
    QLabel* toolLabel_ = nullptr;        // status bar: active tool
    QLabel* toolHintLabel_ = nullptr;    // status bar: what the active tool does
    QLabel* posLabel_ = nullptr;         // status bar: cursor position in document pixels
    QLabel* zoomLabel_ = nullptr;        // status bar: zoom percentage
    QLabel* canvasSizeLabel_ = nullptr;  // options bar: canvas dimensions
    QLabel* zoomValueLabel_ = nullptr;   // options bar: zoom percentage

    QMenu* windowMenu_ = nullptr;         // View-style panel toggles, filled after docks exist
    QToolBar* optionsBar_ = nullptr;      // contextual tool options (top)
    QLabel* optToolName_ = nullptr;       // options bar: active tool name
    QWidget* brushOptions_ = nullptr;     // options bar: brush size/opacity group
    QWidget* wandOptions_ = nullptr;      // options bar: magic-wand group
    QAction* brushOptAction_ = nullptr;   // toolbar action wrapping brushOptions_ (for show/hide)
    QAction* wandOptAction_ = nullptr;    // toolbar action wrapping wandOptions_
    QSpinBox* sizeSpin_ = nullptr;        // options bar: brush diameter
    QSpinBox* opacitySpinOpt_ = nullptr;  // options bar: brush opacity
    QSpinBox* wandTolSpin_ = nullptr;     // options bar: magic-wand tolerance
    QLabel* docTab_ = nullptr;            // document tab strip above the canvas
    QToolButton* fgSwatch_ = nullptr;     // foreground color swatch (tool strip)
    QToolButton* bgSwatch_ = nullptr;     // background color swatch (tool strip)
    QColor fgColor_;                      // current foreground (paint) color
    QColor bgColor_;                      // current background color

    QString currentPath_;
};

}  // namespace pe::app
