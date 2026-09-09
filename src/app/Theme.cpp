#include "Theme.hpp"

#include <QApplication>
#include <QFont>
#include <QPalette>

namespace pe::app {

namespace {

// Every pair below was checked against the WCAG 2.1 relative-luminance formula:
// text and textDim clear 4.5:1 on window/panel/header/base, accent and outline clear
// 3:1 on those same surfaces, and accentText clears 4.5:1 on the accent.
//
// The accent is LIGHT in every theme and carries dark accentText. One accent cannot
// be both dark enough to sit under white text and light enough to read as a focus
// ring on a dark ground; a light accent with dark text satisfies both.

// Graphite (the classic Photoshop medium-neutral grey), on a restrained blue.
const ThemeColors kGraphite{
    /*window*/ QColor(0x2c, 0x2c, 0x2c),     /*panel*/ QColor(0x39, 0x39, 0x39),
    /*header*/ QColor(0x32, 0x32, 0x32),     /*base*/ QColor(0x30, 0x30, 0x30),
    /*raised*/ QColor(0x4d, 0x4d, 0x4d),     /*border*/ QColor(0x23, 0x23, 0x23),
    /*outline*/ QColor(0x85, 0x85, 0x85),    /*text*/ QColor(0xd6, 0xd6, 0xd6),
    /*textDim*/ QColor(0xa8, 0xa8, 0xa8),    /*accent*/ QColor(0x5b, 0x9c, 0xf0),
    /*accentText*/ QColor(0x12, 0x16, 0x1c),
    /*canvas*/ QColor(0x28, 0x28, 0x28),
};

// Charcoal — the darker neutral-grey alternate (Photoshop's darkest setting).
const ThemeColors kSlate{
    /*window*/ QColor(0x1f, 0x1f, 0x1f),     /*panel*/ QColor(0x2a, 0x2a, 0x2a),
    /*header*/ QColor(0x24, 0x24, 0x24),     /*base*/ QColor(0x22, 0x22, 0x22),
    /*raised*/ QColor(0x3c, 0x3c, 0x3c),     /*border*/ QColor(0x16, 0x16, 0x16),
    /*outline*/ QColor(0x7a, 0x7a, 0x7a),    /*text*/ QColor(0xd0, 0xd0, 0xd0),
    /*textDim*/ QColor(0xa0, 0xa0, 0xa0),    /*accent*/ QColor(0x5b, 0x9c, 0xf0),
    /*accentText*/ QColor(0x10, 0x10, 0x10),
    /*canvas*/ QColor(0x1d, 0x1d, 0x1d),
};

// Nocturne (default): the flagship direction, a near-neutral blue-grey ground with
// a single blurple accent used as a line and an indicator, never as a flood. Ground,
// surface, text and accent are the design system's own tokens; the remaining roles
// are steps from its neutral ramp.
const ThemeColors kNocturne{
    /*window*/ QColor(0x16, 0x18, 0x26),     /*panel*/ QColor(0x1c, 0x1f, 0x2c),
    /*header*/ QColor(0x23, 0x25, 0x32),     /*base*/ QColor(0x12, 0x14, 0x1e),
    /*raised*/ QColor(0x3f, 0x42, 0x4d),     /*border*/ QColor(0x0f, 0x11, 0x1a),
    /*outline*/ QColor(0x75, 0x79, 0x8c),    /*text*/ QColor(0xe9, 0xe9, 0xed),
    /*textDim*/ QColor(0x93, 0x97, 0xab),    /*accent*/ QColor(0x91, 0x84, 0xd9),
    /*accentText*/ QColor(0x16, 0x18, 0x26),
    /*canvas*/ QColor(0x10, 0x12, 0x20),
};

ThemeId g_current = ThemeId::Nocturne;

}  // namespace

const ThemeColors& themeColors(ThemeId id) noexcept {
    switch (id) {
        case ThemeId::Graphite:
            return kGraphite;
        case ThemeId::Slate:
            return kSlate;
        case ThemeId::Nocturne:
            return kNocturne;
    }
    return kNocturne;
}

const char* themeName(ThemeId id) noexcept {
    switch (id) {
        case ThemeId::Graphite:
            return "Graphite";
        case ThemeId::Slate:
            return "Charcoal";
        case ThemeId::Nocturne:
            return "Nocturne";
    }
    return "Nocturne";
}

ThemeId themeFromInt(int value) noexcept {
    for (const ThemeId id : kAllThemes) {
        if (value == static_cast<int>(id)) return id;
    }
    return ThemeId::Nocturne;
}

ThemeId currentTheme() noexcept {
    return g_current;
}

QString buildStyleSheet(const ThemeColors& c) {
    // Ultra-dense, refined: tight metrics, hairline grooves, layered value steps for
    // depth, and the accent reserved for thin outlines/indicators on active states.
    QString qss = QStringLiteral(R"QSS(
/* Item views paint their own selection, so they keep the reset. Everything else
   must show keyboard focus; a blanket `* { outline: 0 }` made the app unusable
   from the keyboard even where the wiring worked. */
QAbstractItemView { outline: 0; }
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
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 7px; }
/* Hover previously set the same token the toolbar itself uses, so it was invisible.
   Checked carries an accent outline as well as a fill: the fill alone is a lightness
   step no low-vision user can resolve. */
QToolButton:hover { background: @raised@; }
QToolButton:checked { background: @raised@; border: 1px solid @accent@; }
QToolButton:focus { border: 1px solid @accent@; }

QDockWidget { color: @textDim@; }
QDockWidget > QWidget { background: @panel@; }

QListWidget, QTreeWidget { background: @base@; border: 1px solid @outline@; border-radius: 3px; padding: 2px; }
QListWidget:focus, QTreeWidget:focus { border: 1px solid @accent@; }
QListWidget::item { padding: 4px 7px; border-radius: 3px; color: @text@; margin: 1px 0; }
QListWidget::item:selected { background: @raised@; color: @text@; border: 1px solid @accent@; }
/* The tree has more than one column (the layer, and its mask), and a per-cell border boxes
   EACH cell of the selected row, so a selected layer came out as two boxes with a seam
   between them and its label nudged right by the left edge. Top and bottom only: the cells
   join into one band across the row, and the outline cue the theme relies on is kept. */
QTreeWidget::item:selected { background: @raised@; color: @text@;
    border-top: 1px solid @accent@; border-bottom: 1px solid @accent@; }
QListWidget::item:hover:!selected { background: @raised@; }

QComboBox { background: @base@; border: 1px solid @outline@; border-radius: 3px; padding: 3px 8px; min-height: 20px; }
QComboBox:hover { border-color: @text@; }
QComboBox:focus { border-color: @accent@; }
QComboBox::drop-down { border: 0; width: 18px; }
QComboBox QAbstractItemView { background: @panel@; border: 1px solid @outline@; border-radius: 3px;
    selection-background-color: @accent@; selection-color: @accentText@; padding: 2px; outline: 0; }

QSpinBox { background: @base@; border: 1px solid @outline@; border-radius: 3px; padding: 3px 6px; min-height: 20px; }
QSpinBox:focus { border-color: @accent@; }
QSpinBox::up-button, QSpinBox::down-button { width: 15px; background: @raised@; border: 0; }
QSpinBox::up-button { border-top-right-radius: 2px; }
QSpinBox::down-button { border-bottom-right-radius: 2px; }

QPushButton { background: @raised@; color: @text@; border: 1px solid @outline@; border-radius: 3px;
    padding: 5px 9px; }
QPushButton:hover { background: @raised@; border-color: @text@; }
QPushButton:focus { border: 1px solid @accent@; }
QPushButton:pressed { background: @accent@; color: @accentText@; border-color: @accent@; }
QPushButton:disabled { color: @textDim@; background: @panel@; border-color: @border@; }

QCheckBox:focus, QRadioButton:focus { border: 1px solid @accent@; border-radius: 3px; }
QCheckBox::indicator, QRadioButton::indicator { border: 1px solid @outline@; border-radius: 3px;
    width: 13px; height: 13px; background: @base@; }
QCheckBox::indicator:checked, QRadioButton::indicator:checked { background: @accent@;
    border-color: @accent@; }

QSlider::groove:horizontal { height: 3px; background: @border@; border-radius: 2px; }
QSlider::sub-page:horizontal { background: @accent@; border-radius: 2px; }
QSlider::handle:horizontal { background: @text@; width: 12px; height: 12px; margin: -5px 0; border-radius: 6px; }
QSlider::handle:horizontal:hover { background: @accent@; }
QSlider:focus::handle:horizontal { background: @accent@; border: 1px solid @text@; }

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

/* Tabbed panel groups — the QTabBar is the panel header (Photoshop-style). */
QTabBar { background: @header@; }
/* The selected tab is marked by an accent underline, not by the panel/header
   lightness step: those two tokens sit at about 1.1:1 and cannot carry state. */
QTabBar::tab { background: @header@; color: @textDim@; padding: 5px 11px; border: 0;
    border-right: 1px solid @border@; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { background: @panel@; color: @text@; border-bottom: 2px solid @accent@; }
QTabBar::tab:hover:!selected { color: @text@; background: @raised@; }
QTabBar::tab:focus { border-bottom: 2px solid @accent@; color: @text@; }

/* Options bar (top) and the document tab strip above the canvas. */
QToolBar#OptionsBar { background: @header@; border: 0; border-bottom: 1px solid @border@;
    spacing: 6px; padding: 3px 8px; }
QLabel#OptToolName { color: @text@; font-weight: 600; padding: 0 4px; background: transparent; }
QWidget#DocTabStrip { background: @window@; border-bottom: 1px solid @border@; }
QLabel#DocTab { background: @panel@; color: @text@; padding: 5px 14px;
    border-right: 1px solid @border@; }
QToolBar QLabel { background: transparent; }

/* A panel that is not built yet. Reads as a deliberate empty state rather than as a panel
   that failed to load, which is what a lone centred label looked like. */
QWidget#PanelPlaceholder { background: @panel@; }
QLabel#PlaceholderTitle { color: @text@; font-weight: 600; background: transparent; }
QLabel#PlaceholderBody { color: @textDim@; background: transparent; }
QLabel#PlaceholderNote { color: @textDim@; font-style: italic; background: transparent; }
QWidget#SwatchesPanel { background: @base@; }
)QSS");

    struct Tok {
        const char* name;
        QColor value;
    };
    const Tok toks[] = {
        {"@window@", c.window},         {"@panel@", c.panel},
        {"@header@", c.header},         {"@base@", c.base},
        {"@raised@", c.raised},         {"@border@", c.border},
        {"@outline@", c.outline},       {"@text@", c.text},
        {"@textDim@", c.textDim},       {"@accent@", c.accent},
        {"@accentText@", c.accentText},
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
