#pragma once

#include <QMainWindow>

namespace pe::app {

// The top-level application window. At this skeleton stage it establishes the
// dockable-panel layout that the full UI (layers, tools, history, properties,
// color) will populate. See docs/systems/24-ui-workspace.md.
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void buildMenuBar();
    void buildDockPanels();
    void buildStatusBar();
};

}  // namespace pe::app
