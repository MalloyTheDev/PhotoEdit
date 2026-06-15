#include "MainWindow.hpp"

#include "CanvasView.hpp"
#include "HistoryPanel.hpp"
#include "IconUtil.hpp"
#include "LayersPanel.hpp"
#include "PanelHeader.hpp"
#include "pe/core/Document.hpp"
#include "pe/core/DocumentIO.hpp"
#include "pe/core/Version.hpp"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QColor>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QRectF>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace pe::app {

namespace {
// File-dialog filter covering the formats the engine can read/write.
constexpr const char* kOpenFilter =
    "Images (*.pedoc *.png *.jpg *.jpeg *.tif *.tiff *.webp);;All files (*)";
constexpr const char* kSaveFilter =
    "PhotoEdit document (*.pedoc);;PNG (*.png);;JPEG (*.jpg);;TIFF (*.tif);;WebP (*.webp)";

// A calm light tint for resting tool icons (the active one is marked by an accent
// outline, so the glyph itself stays restrained).
const QColor kToolIconColor(0xbe, 0xc4, 0xcc);

// The brand monogram pixmap: an accent-filled rounded tile with a "P".
[[nodiscard]] QPixmap brandPixmap(const ThemeColors& c) {
    constexpr int kSize = 30;
    constexpr qreal kDpr = 2.0;
    QPixmap pm(static_cast<int>(kSize * kDpr), static_cast<int>(kSize * kDpr));
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(kDpr);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(c.accent);
    p.drawRoundedRect(QRectF(0, 0, kSize, kSize), 8, 8);
    QFont bf;
    bf.setPointSizeF(13.0);
    bf.setBold(true);
    p.setFont(bf);
    p.setPen(c.accentText);
    p.drawText(QRectF(0, -1, kSize, kSize), Qt::AlignCenter, QStringLiteral("P"));
    p.end();
    return pm;
}

// A compact brand monogram for the top of the tool strip — the app's in-window identity.
[[nodiscard]] QLabel* makeBrandMark(QWidget* parent) {
    auto* mark = new QLabel(parent);
    mark->setObjectName(QStringLiteral("BrandMark"));
    mark->setPixmap(brandPixmap(themeColors(currentTheme())));
    mark->setAlignment(Qt::AlignCenter);
    mark->setToolTip(QStringLiteral("PhotoEdit"));
    mark->setContentsMargins(0, 2, 0, 6);
    return mark;
}

// One tool-strip entry. `tool` == Inactive marks a scaffolded, not-yet-wired tool.
struct ToolDef {
    const char* icon;
    const char* label;
    CanvasView::Tool tool;
    const char* shortcut;
};
}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    resize(1280, 800);

    canvas_ = new CanvasView(this);
    setCentralWidget(canvas_);

    buildMenuBar();
    buildToolBar();
    buildDockPanels();
    buildStatusBar();
    refreshTitle();
}

MainWindow::~MainWindow() {
    // Detach the observing widgets while doc_ is still alive: doc_ (a member) is
    // destroyed when this body returns, but the child widgets are deleted later by
    // the QObject base destructor, so clearing them now avoids a dangling
    // removeObserver() in their destructors.
    if (canvas_ != nullptr) canvas_->setDocument(nullptr);
    if (layers_ != nullptr) layers_->setDocument(nullptr);
    if (history_ != nullptr) history_->setDocument(nullptr);
}

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
    QAction* undoAct = editMenu->addAction(QStringLiteral("&Undo"), this, &MainWindow::undo);
    undoAct->setShortcut(QKeySequence::Undo);
    QAction* redoAct = editMenu->addAction(QStringLiteral("&Redo"), this, &MainWindow::redo);
    redoAct->setShortcut(QKeySequence::Redo);

    menuBar()->addMenu(QStringLiteral("&Image"));
    menuBar()->addMenu(QStringLiteral("&Layer"));
    menuBar()->addMenu(QStringLiteral("&Select"));
    menuBar()->addMenu(QStringLiteral("F&ilter"));

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
    const struct {
        ThemeId id;
        const char* label;
    } themes[] = {{ThemeId::Graphite, "Graphite"}, {ThemeId::Slate, "Slate"}};
    for (const auto& t : themes) {
        QAction* a = themeMenu->addAction(QString::fromUtf8(t.label));
        a->setCheckable(true);
        a->setChecked(currentTheme() == t.id);
        themeGroup->addAction(a);
        const ThemeId id = t.id;
        connect(a, &QAction::triggered, this, [this, id] { setTheme(id); });
    }

    menuBar()->addMenu(QStringLiteral("&Window"));
    menuBar()->addMenu(QStringLiteral("&Help"));
}

void MainWindow::buildToolBar() {
    auto* tb = new QToolBar(QStringLiteral("Tools"), this);
    tb->setMovable(false);
    tb->setFloatable(false);
    tb->setIconSize(QSize(22, 22));
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);
    addToolBar(Qt::LeftToolBarArea, tb);

    brandMark_ = makeBrandMark(tb);  // app identity at the top of the strip
    tb->addWidget(brandMark_);
    tb->addSeparator();

    // Grouped like a pro editor: select · crop/sample · paint · type · navigate.
    // Brush/Eraser/Hand/Zoom are wired; the rest are scaffolded (Inactive) for now.
    using Tool = CanvasView::Tool;
    const std::vector<std::vector<ToolDef>> groups = {
        {{"move", "Move", Tool::Inactive, "V"},
         {"marquee", "Rectangular Marquee", Tool::Inactive, "M"},
         {"lasso", "Lasso", Tool::Inactive, "L"},
         {"wand-sparkles", "Magic Wand", Tool::Inactive, "W"}},
        {{"crop", "Crop", Tool::Inactive, "C"}, {"pipette", "Eyedropper", Tool::Inactive, "I"}},
        {{"paintbrush", "Brush", Tool::Brush, "B"},
         {"eraser", "Eraser", Tool::Eraser, "E"},
         {"paint-bucket", "Paint Bucket", Tool::Inactive, "G"}},
        {{"type", "Type", Tool::Inactive, "T"}},
        {{"hand", "Hand", Tool::Hand, "H"}, {"zoom-in", "Zoom", Tool::Zoom, "Z"}},
    };

    auto* toolGroup = new QActionGroup(this);
    QAction* brushAction = nullptr;
    for (std::size_t g = 0; g < groups.size(); ++g) {
        if (g > 0) tb->addSeparator();
        for (const ToolDef& def : groups[g]) {
            QAction* a =
                tb->addAction(renderIconAsIcon(QString::fromUtf8(def.icon), kToolIconColor, 22),
                              QString::fromUtf8(def.label));
            a->setCheckable(true);
            a->setActionGroup(toolGroup);
            const bool wired = def.tool != Tool::Inactive;
            a->setToolTip(QStringLiteral("%1  (%2)%3")
                              .arg(QString::fromUtf8(def.label), QString::fromUtf8(def.shortcut),
                                   wired ? QString() : QStringLiteral("  — coming soon")));
            a->setShortcut(QKeySequence(QString::fromUtf8(def.shortcut)));
            const Tool tool = def.tool;
            const QString label = QString::fromUtf8(def.label);
            connect(a, &QAction::triggered, this, [this, tool, label, wired] {
                canvas_->setTool(tool);
                toolLabel_->setText(wired ? label
                                          : QStringLiteral("%1 — not yet implemented").arg(label));
            });
            if (def.tool == Tool::Brush) brushAction = a;
        }
    }
    if (brushAction != nullptr) brushAction->setChecked(true);  // default tool
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

void MainWindow::undo() {
    if (doc_ == nullptr) return;
    // A live brush stroke applies an uncommitted preview straight to the tiles
    // (outside history); mutating history underneath it would desync the preview's
    // snapshots. Ignore undo/redo until the stroke is committed.
    if (canvas_ != nullptr && canvas_->tool().isStroking()) return;
    doc_->history().undo();  // notifies -> canvas refreshes
}

void MainWindow::redo() {
    if (doc_ == nullptr) return;
    if (canvas_ != nullptr && canvas_->tool().isStroking()) return;
    doc_->history().redo();  // notifies -> canvas refreshes
}

void MainWindow::setDocument(std::unique_ptr<pe::Document> doc, QString path) {
    // Detach the observing widgets from the outgoing document before it is destroyed.
    canvas_->setDocument(nullptr);
    if (layers_ != nullptr) layers_->setDocument(nullptr);
    if (history_ != nullptr) history_->setDocument(nullptr);
    doc_ = std::move(doc);
    currentPath_ = std::move(path);
    canvas_->setDocument(doc_.get());
    if (layers_ != nullptr) layers_->setDocument(doc_.get());
    if (history_ != nullptr) history_->setDocument(doc_.get());
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
        const char* icon;
        Qt::DockWidgetArea area;
    } panels[] = {
        {"Layers", "layers", Qt::RightDockWidgetArea},
        {"Channels", "blend", Qt::RightDockWidgetArea},
        {"Properties", "sliders-horizontal", Qt::RightDockWidgetArea},
        {"History", "history", Qt::RightDockWidgetArea},
        {"Color", "palette", Qt::RightDockWidgetArea},
        // the left tool strip replaces the old Tools dock
    };
    for (const auto& p : panels) {
        auto* dock = new QDockWidget(QString::fromUtf8(p.title), this);
        dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
        dock->setTitleBarWidget(
            makePanelHeader(QString::fromUtf8(p.title), QString::fromUtf8(p.icon), dock));
        if (std::string_view(p.title) == "Layers") {
            layers_ = new LayersPanel(dock);
            dock->setWidget(layers_);
        } else if (std::string_view(p.title) == "History") {
            history_ = new HistoryPanel(dock);
            dock->setWidget(history_);
        } else {
            auto* placeholder = new QLabel(QStringLiteral("%1").arg(p.title));
            placeholder->setObjectName(QStringLiteral("PanelPlaceholder"));
            placeholder->setAlignment(Qt::AlignCenter);
            dock->setWidget(placeholder);
        }
        addDockWidget(p.area, dock);
    }
}

void MainWindow::buildStatusBar() {
    toolLabel_ = new QLabel(QStringLiteral("Brush"), this);
    zoomLabel_ = new QLabel(QStringLiteral("—"), this);
    statusBar()->addWidget(toolLabel_);
    statusBar()->addPermanentWidget(zoomLabel_);
    connect(canvas_, &CanvasView::zoomChanged, this,
            [this](double pct) { zoomLabel_->setText(QStringLiteral("%1%").arg(pct, 0, 'f', 0)); });
}

void MainWindow::setTheme(ThemeId id) {
    applyTheme(*qApp, id);
    if (brandMark_ != nullptr)
        brandMark_->setPixmap(brandPixmap(themeColors(id)));  // recolor accent
    if (canvas_ != nullptr) canvas_->update();                // repaint the themed pasteboard
    QSettings().setValue(QStringLiteral("theme"), static_cast<int>(id));
}

}  // namespace pe::app
