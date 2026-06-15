#include "Theme.hpp"

#include <QApplication>
#include <QFont>
#include <QPalette>

namespace pe::app {

namespace {

// Graphite — neutral grey, calm blue accent (Photoshop-family).
const ThemeColors kGraphite{
    /*window*/ QColor(0x2b, 0x2b, 0x2b), /*panel*/ QColor(0x33, 0x33, 0x33),
    /*header*/ QColor(0x3c, 0x3c, 0x3c), /*base*/ QColor(0x25, 0x25, 0x25),
    /*raised*/ QColor(0x47, 0x47, 0x47), /*border*/ QColor(0x20, 0x20, 0x20),
    /*text*/ QColor(0xd7, 0xd7, 0xd7),   /*textDim*/ QColor(0x8c, 0x8c, 0x8c),
    /*accent*/ QColor(0x2f, 0x80, 0xed), /*accentText*/ QColor(0xff, 0xff, 0xff),
    /*canvas*/ QColor(0x1e, 0x1e, 0x1e),
};

// Slate — distinctly cool blue-grey, warm amber accent (more opinionated, AE-family).
const ThemeColors kSlate{
    /*window*/ QColor(0x1b, 0x23, 0x2e), /*panel*/ QColor(0x28, 0x32, 0x3f),
    /*header*/ QColor(0x31, 0x3d, 0x4d), /*base*/ QColor(0x16, 0x1d, 0x26),
    /*raised*/ QColor(0x3a, 0x48, 0x59), /*border*/ QColor(0x11, 0x16, 0x1d),
    /*text*/ QColor(0xd5, 0xdb, 0xe3),   /*textDim*/ QColor(0x7d, 0x8a, 0x9c),
    /*accent*/ QColor(0xeb, 0x92, 0x34), /*accentText*/ QColor(0x15, 0x18, 0x1d),
    /*canvas*/ QColor(0x13, 0x19, 0x22),
};

ThemeId g_current = ThemeId::Graphite;

}  // namespace

const ThemeColors& themeColors(ThemeId id) noexcept {
    return id == ThemeId::Slate ? kSlate : kGraphite;
}

const char* themeName(ThemeId id) noexcept {
    return id == ThemeId::Slate ? "Slate" : "Graphite";
}

ThemeId currentTheme() noexcept {
    return g_current;
}

QString buildStyleSheet(const ThemeColors& c) {
    // A token-replaced template keeps the two themes structurally identical.
    QString qss = QStringLiteral(R"QSS(
* { outline: 0; }
QWidget { background: @panel@; color: @text@; }
QMainWindow, QMainWindow > QWidget { background: @window@; }
QMainWindow::separator { background: @border@; width: 1px; height: 1px; }
QToolTip { background: @header@; color: @text@; border: 1px solid @border@; padding: 4px 6px; }

QMenuBar { background: @header@; color: @text@; padding: 3px 6px; border-bottom: 1px solid @border@; }
QMenuBar::item { background: transparent; padding: 5px 10px; border-radius: 5px; }
QMenuBar::item:selected { background: @raised@; }
QMenuBar::item:pressed { background: @accent@; color: @accentText@; }

QMenu { background: @panel@; color: @text@; border: 1px solid @border@; padding: 5px; }
QMenu::item { padding: 6px 28px 6px 14px; border-radius: 5px; }
QMenu::item:selected { background: @accent@; color: @accentText@; }
QMenu::item:disabled { color: @textDim@; }
QMenu::separator { height: 1px; background: @border@; margin: 5px 10px; }

QToolBar { background: @header@; border: 0; border-right: 1px solid @border@; padding: 8px 6px; spacing: 4px; }
QToolBar::separator { background: @border@; height: 1px; margin: 7px 5px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 7px; padding: 7px; }
QToolButton:hover { background: @raised@; }
QToolButton:checked { background: @accent@; border-color: @accent@; }

QDockWidget { color: @textDim@; font-size: 10px; font-weight: 600; }
QDockWidget::title { background: @header@; padding: 8px 12px; border-bottom: 1px solid @border@; }
QDockWidget > QWidget { background: @panel@; }

QListWidget { background: @base@; border: 1px solid @border@; border-radius: 7px; padding: 3px; }
QListWidget::item { padding: 6px 8px; border-radius: 5px; color: @text@; }
QListWidget::item:selected { background: @accent@; color: @accentText@; }
QListWidget::item:hover:!selected { background: @raised@; }

QComboBox { background: @base@; border: 1px solid @border@; border-radius: 7px; padding: 5px 10px; min-height: 22px; }
QComboBox:hover { border-color: @raised@; }
QComboBox:focus { border-color: @accent@; }
QComboBox::drop-down { border: 0; width: 20px; }
QComboBox QAbstractItemView { background: @panel@; border: 1px solid @border@; border-radius: 6px;
    selection-background-color: @accent@; selection-color: @accentText@; padding: 3px; outline: 0; }

QSpinBox { background: @base@; border: 1px solid @border@; border-radius: 7px; padding: 4px 8px; min-height: 22px; }
QSpinBox:focus { border-color: @accent@; }
QSpinBox::up-button, QSpinBox::down-button { width: 16px; background: @raised@; border: 0; }
QSpinBox::up-button { border-top-right-radius: 6px; }
QSpinBox::down-button { border-bottom-right-radius: 6px; }

QPushButton { background: @raised@; color: @text@; border: 1px solid @border@; border-radius: 7px;
    padding: 6px 12px; font-weight: 500; }
QPushButton:hover { border-color: @accent@; }
QPushButton:pressed { background: @accent@; color: @accentText@; border-color: @accent@; }
QPushButton:disabled { color: @textDim@; background: @panel@; border-color: @border@; }

QScrollBar:vertical { background: transparent; width: 12px; margin: 0; }
QScrollBar::handle:vertical { background: @raised@; border-radius: 5px; min-height: 32px; margin: 2px; }
QScrollBar::handle:vertical:hover { background: @textDim@; }
QScrollBar:horizontal { background: transparent; height: 12px; margin: 0; }
QScrollBar::handle:horizontal { background: @raised@; border-radius: 5px; min-width: 32px; margin: 2px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QStatusBar { background: @header@; color: @textDim@; border-top: 1px solid @border@; }
QStatusBar::item { border: 0; }
QStatusBar QLabel { color: @textDim@; padding: 0 10px; }

QLabel#PanelPlaceholder { color: @textDim@; font-weight: 500; }
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
    p.setColor(QPalette::ToolTipBase, c.header);
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
    f.setPointSizeF(9.5);  // dense, pro-tool scale
    app.setFont(f);

    app.setStyleSheet(buildStyleSheet(c));
}

}  // namespace pe::app
