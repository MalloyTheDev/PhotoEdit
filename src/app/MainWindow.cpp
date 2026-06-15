#include "MainWindow.hpp"

#include "CanvasView.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/Version.hpp"

#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>

#include <string>

namespace pe::app {

namespace {
// File-dialog filter covering the formats the engine can read/write.
constexpr const char* kOpenFilter =
    "Images (*.pedoc *.png *.jpg *.jpeg *.tif *.tiff *.webp);;All files (*)";
constexpr const char* kSaveFilter =
    "PhotoEdit document (*.pedoc);;PNG (*.png);;JPEG (*.jpg);;TIFF (*.tif);;WebP (*.webp)";
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    resize(1280, 800);

    canvas_ = new CanvasView(this);
    setCentralWidget(canvas_);

    buildMenuBar();
    buildDockPanels();
    buildStatusBar();
    refreshTitle();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildMenuBar() {
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));
    fileMenu->addAction(QStringLiteral("&New"), this, &MainWindow::newDocument);
    fileMenu->addAction(QStringLiteral("&Open..."), this, &MainWindow::openDocument);
    fileMenu->addSeparator();
    fileMenu->addAction(QStringLiteral("&Save"), this, &MainWindow::saveDocument);
    fileMenu->addAction(QStringLiteral("Save &As..."), this, &MainWindow::saveDocumentAs);
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

void MainWindow::newDocument() {
    setDocument(pe::Document::createBlank(pe::Size{800, 600}), QString());
    statusBar()->showMessage(QStringLiteral("New 800x600 document"), 3000);
}

void MainWindow::openDocument() {
    const QString path =
        QFileDialog::getOpenFileName(this, QStringLiteral("Open Image"), QString(), kOpenFilter);
    if (path.isEmpty()) return;

    auto doc = pe::loadDocument(path.toStdString());
    if (doc == nullptr) {
        QMessageBox::warning(this, QStringLiteral("Open failed"),
                             QStringLiteral("Could not open \"%1\".").arg(path));
        return;
    }
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
    currentPath_ = path;
    refreshTitle();
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 3000);
    return true;
}

void MainWindow::setDocument(std::unique_ptr<pe::Document> doc, QString path) {
    doc_ = std::move(doc);
    currentPath_ = std::move(path);
    canvas_->showDocument(doc_.get());
    refreshTitle();
}

void MainWindow::refreshTitle() {
    const QString name =
        currentPath_.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(currentPath_).fileName();
    setWindowTitle(QStringLiteral("%1 — PhotoEdit %2")
                       .arg(doc_ ? name : QStringLiteral("(no document)"))
                       .arg(pe::Version::string()));
}

void MainWindow::buildDockPanels() {
    const struct {
        const char* title;
        Qt::DockWidgetArea area;
    } panels[] = {
        {"Layers", Qt::RightDockWidgetArea},     {"Channels", Qt::RightDockWidgetArea},
        {"Properties", Qt::RightDockWidgetArea}, {"History", Qt::RightDockWidgetArea},
        {"Color", Qt::RightDockWidgetArea},      {"Tools", Qt::LeftDockWidgetArea},
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

}  // namespace pe::app
