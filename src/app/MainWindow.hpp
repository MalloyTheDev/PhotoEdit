#pragma once

#include "Theme.hpp"

#include <QMainWindow>
#include <QString>

#include <memory>

class QLabel;

namespace pe {
class Document;
}

namespace pe::app {

class CanvasView;
class LayersPanel;
class HistoryPanel;

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
    void buildDockPanels();
    void buildStatusBar();
    void setTheme(ThemeId id);

    void newDocument();
    void openDocument();
    bool saveDocument();    // saves to the current path, or prompts if none
    bool saveDocumentAs();  // always prompts
    void undo();
    void redo();
    bool writeTo(const QString& path);
    void setDocument(std::unique_ptr<pe::Document> doc, QString path);
    void refreshTitle();

    std::unique_ptr<pe::Document> doc_;
    CanvasView* canvas_ = nullptr;
    LayersPanel* layers_ = nullptr;
    HistoryPanel* history_ = nullptr;
    QLabel* toolLabel_ = nullptr;  // status bar: active tool
    QLabel* zoomLabel_ = nullptr;  // status bar: zoom percentage
    QString currentPath_;
};

}  // namespace pe::app
