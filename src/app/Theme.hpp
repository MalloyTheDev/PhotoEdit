#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace pe::app {

// PhotoEdit's two dark "pro editor" visual directions. Graphite is a neutral,
// Photoshop-style grey with a calm blue accent; Slate is a cooler blue-grey with a
// warm amber accent. Both are applied as a QPalette + a generated QSS stylesheet on
// the Fusion style. See docs (design system) — this is the app's visual vocabulary.
enum class ThemeId { Graphite, Slate };

// The full color vocabulary a theme is built from. Everything in the QSS and the
// canvas pasteboard derives from these eleven roles.
struct ThemeColors {
    QColor window;      // outermost app background
    QColor panel;       // dock / panel body
    QColor header;      // panel header, menu bar, tool strip, status bar
    QColor base;        // inset surfaces: lists, inputs
    QColor raised;      // hover / control fill
    QColor border;      // hairline separators (a groove, darker than panel)
    QColor text;        // primary text
    QColor textDim;     // secondary text, headers, disabled
    QColor accent;      // active tool, selection, focus
    QColor accentText;  // text/!icon on an accent fill
    QColor canvas;      // the pasteboard behind the document
};

[[nodiscard]] const ThemeColors& themeColors(ThemeId id) noexcept;
[[nodiscard]] const char* themeName(ThemeId id) noexcept;

// The theme currently applied (process-wide), so widgets like CanvasView can read
// the pasteboard color without threading it through every constructor.
[[nodiscard]] ThemeId currentTheme() noexcept;

// Build the application-wide QSS for a palette.
[[nodiscard]] QString buildStyleSheet(const ThemeColors& c);

// Apply `id` to the whole application: Fusion style, QPalette, app font, and QSS.
// Records it as the current theme.
void applyTheme(QApplication& app, ThemeId id);

}  // namespace pe::app
