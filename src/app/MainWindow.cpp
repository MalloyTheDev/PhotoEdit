#include "MainWindow.hpp"

#include "pe/core/Version.hpp"

#include <QApplication>
#include <QDockWidget>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>

namespace pe::app {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("PhotoEdit %1").arg(pe::Version::string()));
    resize(1280, 800);

    buildMenuBar();
    buildDockPanels();
    buildStatusBar();

    // Placeholder canvas area. The real tile-based canvas viewport replaces this
    // in the canvas/rendering milestone (docs/systems/02-canvas-rendering.md).
    auto* canvasPlaceholder = new QLabel(
        QStringLiteral("Canvas viewport (tile renderer lands in M2)"), this);
    canvasPlaceholder->setAlignment(Qt::AlignCenter);
    canvasPlaceholder->setMinimumSize(640, 480);
    setCentralWidget(canvasPlaceholder);
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenuBar() {
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&New..."));
    fileMenu->addAction(QStringLiteral("&Open..."));
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("E&xit"), qApp, &QApplication::quit);

    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    editMenu->addAction(QStringLiteral("&Undo"));
    editMenu->addAction(QStringLiteral("&Redo"));

    menuBar()->addMenu(QStringLiteral("&Image"));
    menuBar()->addMenu(QStringLiteral("&Layer"));
    menuBar()->addMenu(QStringLiteral("&Select"));
    menuBar()->addMenu(QStringLiteral("F&ilter"));
    menuBar()->addMenu(QStringLiteral("&View"));
    menuBar()->addMenu(QStringLiteral("&Window"));
    menuBar()->addMenu(QStringLiteral("&Help"));
}

void MainWindow::buildDockPanels() {
    // Right-hand panel stack mirrors the canonical Photoshop layout. Each is a
    // placeholder until its owning system is implemented.
    const struct {
        const char* title;
        Qt::DockWidgetArea area;
    } panels[] = {
        {"Layers", Qt::RightDockWidgetArea},
        {"Channels", Qt::RightDockWidgetArea},
        {"Properties", Qt::RightDockWidgetArea},
        {"History", Qt::RightDockWidgetArea},
        {"Color", Qt::RightDockWidgetArea},
        {"Tools", Qt::LeftDockWidgetArea},
    };

    for (const auto& p : panels) {
        auto* dock = new QDockWidget(QString::fromUtf8(p.title), this);
        dock->setWidget(new QLabel(QStringLiteral("(%1 panel)").arg(p.title)));
        addDockWidget(p.area, dock);
    }
}

void MainWindow::buildStatusBar() {
    statusBar()->showMessage(QStringLiteral("Ready"));
}

} // namespace pe::app
