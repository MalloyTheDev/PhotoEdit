#include "Theme.hpp"

#include <QApplication>
#include <QFont>
#include <QPalette>

namespace pe::app {

namespace {

// Slate (default) — a refined, restrained cool blue-grey with a quiet amber accent.
// Value steps (canvas < base < window < header < panel < raised) build soft depth
// without shadows; the accent is used sparingly as an outline/indicator, not a fill.
const ThemeColors kSlate{
    /*window*/ QColor(0x19, 0x1e, 0x26), /*panel*/ QColor(0x22, 0x29, 0x32),
    /*header*/ QColor(0x1d, 0x23, 0x2b), /*base*/ QColor(0x15, 0x1a, 0x21),
    /*raised*/ QColor(0x2c, 0x35, 0x41), /*border*/ QColor(0x10, 0x14, 0x1a),
    /*text*/ QColor(0xcc, 0xd3, 0xdd),   /*textDim*/ QColor(0x71, 0x7f, 0x90),
    /*accent*/ QColor(0xd9, 0x8a, 0x3d), /*accentText*/ QColor(0x14, 0x17, 0x1b),
    /*canvas*/ QColor(0x10, 0x14, 0x1a),
};

// Graphite — the neutral-grey alternate, calm blue accent (Photoshop-family).
const ThemeColors kGraphite{
    /*window*/ QColor(0x23, 0x23, 0x23), /*panel*/ QColor(0x2b, 0x2b, 0x2b),
    /*header*/ QColor(0x26, 0x26, 0x26), /*base*/ QColor(0x1c, 0x1c, 0x1c),
    /*raised*/ QColor(0x36, 0x36, 0x36), /*border*/ QColor(0x16, 0x16, 0x16),
    /*text*/ QColor(0xcf, 0xcf, 0xcf),   /*textDim*/ QColor(0x7d, 0x7d, 0x7d),
    /*accent*/ QColor(0x4a, 0x90, 0xd9), /*accentText*/ QColor(0xff, 0xff, 0xff),
    /*canvas*/ QColor(0x18, 0x18, 0x18),
};

ThemeId g_current = ThemeId::Slate;

}  // namespace

const ThemeColors& themeColors(ThemeId id) noexcept {
    return id == ThemeId::Graphite ? kGraphite : kSlate;
}

const char* themeName(ThemeId id) noexcept {
    return id == ThemeId::Graphite ? "Graphite" : "Slate";
}

ThemeId currentTheme() noexcept {
    return g_current;
}

QString buildStyleSheet(const ThemeColors& c) {
    // Ultra-dense, refined: tight metrics, hairline grooves, layered value steps for
    // depth, and the accent reserved for thin outlines/indicators on active states.
    QString qss = QStringLiteral(R"QSS(
* { outline: 0; }
QWidget { background: @panel@; color: @text@; font-size: 12px; }
QMainWindow, QMainWindow > QWidget { background: @window@; }
QMainWindow::separator { background: @border@; width: 1px; height: 1px; }
QToolTip { background: @raised@; color: @text@; border: 1px solid @border@; padding: 3px 6px; }
QAbstractScrollArea { border: 0; }

QMenuBar { background: @window@; color: @text@; padding: 2px 4px; border-bottom: 1px solid @border@; }
QMenuBar::item { background: transparent; padding: 4px 9px; border-radius: 4px; }
QMenuBar::item:selected { background: @raised@; }
QMenuBar::item:pressed { background: @raised@; color: @text@; }

QMenu { background: @panel@; color: @text@; border: 1px solid @border@; padding: 4px; }
QMenu::item { padding: 5px 26px 5px 12px; border-radius: 4px; }
QMenu::item:selected { background: @accent@; color: @accentText@; }
QMenu::item:disabled { color: @textDim@; }
QMenu::separator { height: 1px; background: @border@; margin: 4px 8px; }

QToolBar { background: @header@; border: 0; border-right: 1px solid @border@; padding: 6px 6px; spacing: 3px; }
QToolBar::separator { background: @border@; height: 1px; margin: 6px 6px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px; padding: 7px; }
QToolButton:hover { background: @raised@; }
QToolButton:checked { background: @raised@; border: 1px solid @accent@; }

QDockWidget { color: @textDim@; }
QDockWidget > QWidget { background: @panel@; }

QListWidget { background: @base@; border: 1px solid @border@; border-radius: 6px; padding: 2px; }
QListWidget::item { padding: 4px 7px; border-radius: 4px; color: @text@; margin: 1px 0; }
QListWidget::item:selected { background: @raised@; color: @text@; border-left: 3px solid @accent@; }
QListWidget::item:hover:!selected { background: @header@; }

QComboBox { background: @base@; border: 1px solid @border@; border-radius: 5px; padding: 3px 8px; min-height: 20px; }
QComboBox:hover { border-color: @raised@; }
QComboBox:focus { border-color: @accent@; }
QComboBox::drop-down { border: 0; width: 18px; }
QComboBox QAbstractItemView { background: @panel@; border: 1px solid @border@; border-radius: 5px;
    selection-background-color: @accent@; selection-color: @accentText@; padding: 2px; outline: 0; }

QSpinBox { background: @base@; border: 1px solid @border@; border-radius: 5px; padding: 3px 6px; min-height: 20px; }
QSpinBox:focus { border-color: @accent@; }
QSpinBox::up-button, QSpinBox::down-button { width: 15px; background: @raised@; border: 0; }
QSpinBox::up-button { border-top-right-radius: 4px; }
QSpinBox::down-button { border-bottom-right-radius: 4px; }

QPushButton { background: @raised@; color: @text@; border: 1px solid @border@; border-radius: 5px;
    padding: 5px 9px; }
QPushButton:hover { border-color: @accent@; }
QPushButton:pressed { background: @accent@; color: @accentText@; border-color: @accent@; }
QPushButton:disabled { color: @textDim@; background: @panel@; border-color: @border@; }

QSlider::groove:horizontal { height: 3px; background: @border@; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent@; border-radius: 2px; }
QSlider::handle:horizontal { background: @text@; width: 12px; height: 12px; margin: -5px 0; border-radius: 6px; }
QSlider::handle:horizontal:hover { background: @accent@; }

QScrollBar:vertical { background: transparent; width: 11px; margin: 0; }
QScrollBar::handle:vertical { background: @raised@; border-radius: 4px; min-height: 28px; margin: 2px; }
QScrollBar::handle:vertical:hover { background: @textDim@; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 0; }
QScrollBar::handle:horizontal { background: @raised@; border-radius: 4px; min-width: 28px; margin: 2px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QStatusBar { background: @header@; color: @textDim@; border-top: 1px solid @border@; }
QStatusBar::item { border: 0; }
QStatusBar QLabel { color: @textDim@; padding: 0 9px; }

QWidget#PanelHeaderBar { background: @header@; border-bottom: 1px solid @border@; }
QLabel#PanelHeaderLabel { color: @textDim@; background: transparent; }
QLabel#BrandMark { background: transparent; }
QToolBar QLabel { background: transparent; }
)QSS");

    struct Tok {
        const char* name;
        QColor value;
    };
    const Tok toks[] = {
        {"@window@", c.window}, {"@panel@", c.panel},
        {"@header@", c.header}, {"@base@", c.base},
        {"@raised@", c.raised}, {"@border@", c.border},
        {"@text@", c.text},     {"@textDim@", c.textDim},
        {"@accent@", c.accent}, {"@accentText@", c.accentText},
    };
    for (const Tok& t : toks) qss.replace(QLatin1String(t.name), t.value.name());
    return qss;
}

void applyTheme(QApplication& app, ThemeId id) {
    g_current = id;
    app.setStyle(QStringLiteral("Fusion"));  // honors QPalette/QSS consistently

    const ThemeColors& c = themeColors(id);

    QPalette p;
    p.setColor(QPalette::Window, c.panel);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.base);
    p.setColor(QPalette::AlternateBase, c.panel);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.raised);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::ToolTipBase, c.raised);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText, c.accentText);
    p.setColor(QPalette::PlaceholderText, c.textDim);
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::Disabled, QPalette::Text, c.textDim);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.textDim);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.textDim);
    app.setPalette(p);

    QFont f = app.font();
#ifdef Q_OS_WIN
    f.setFamily(QStringLiteral("Segoe UI"));
#endif
    f.setPointSizeF(9.0);  // ultra-dense, pro-tool scale
    app.setFont(f);

    app.setStyleSheet(buildStyleSheet(c));
}

}  // namespace pe::app
