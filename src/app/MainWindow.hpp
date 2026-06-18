#pragma once

#include "Theme.hpp"

#include <QMainWindow>
#include <QString>

#include <memory>

class QAction;
class QLabel;
class QSpinBox;
class QToolBar;
class QToolButton;
class QWidget;

namespace pe {
class Document;
}

namespace pe::app {

class CanvasView;
class LayersPanel;
class HistoryPanel;
class ColorPanel;
class PropertiesPanel;

// The top-level application window. Wires the File menu to the engine's document I/O
// and shows the active document on a CanvasView. The dockable panels (layers, tools,
// history, ...) are still placeholders. See docs/systems/24-ui-workspace.md.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void buildMenuBar();
    void buildToolBar();
    void buildOptionsBar();
    void buildCentral();
    void buildDockPanels();
    void buildStatusBar();
    void setTheme(ThemeId id);

    // Which contextual control group the options bar shows for the active tool.
    enum class OptKind { None, Brush, Move };
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
    QLabel* toolLabel_ = nullptr;  // status bar: active tool
    QLabel* zoomLabel_ = nullptr;  // status bar: zoom percentage

    QToolBar* optionsBar_ = nullptr;      // contextual tool options (top)
    QLabel* optToolName_ = nullptr;       // options bar: active tool name
    QWidget* brushOptions_ = nullptr;     // options bar: brush size/opacity group
    QWidget* moveOptions_ = nullptr;      // options bar: move-tool group
    QAction* brushOptAction_ = nullptr;   // toolbar action wrapping brushOptions_ (for show/hide)
    QAction* moveOptAction_ = nullptr;    // toolbar action wrapping moveOptions_
    QSpinBox* sizeSpin_ = nullptr;        // options bar: brush diameter
    QSpinBox* opacitySpinOpt_ = nullptr;  // options bar: brush opacity
    QLabel* docTab_ = nullptr;            // document tab strip above the canvas
    QToolButton* fgSwatch_ = nullptr;     // foreground color swatch (tool strip)
    QToolButton* bgSwatch_ = nullptr;     // background color swatch (tool strip)
    QColor fgColor_;                      // current foreground (paint) color
    QColor bgColor_;                      // current background color

    QString currentPath_;
};

}  // namespace pe::app
