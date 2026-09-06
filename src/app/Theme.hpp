#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace pe::app {

// PhotoEdit's dark "pro editor" visual directions. Graphite is a neutral,
// Photoshop-style grey; Charcoal is its darker sibling; Nocturne is the flagship
// blue-grey ground with a blurple accent. All are applied as a QPalette + a
// generated QSS stylesheet on the Fusion style.
//
// The enumerator order is persisted in QSettings, so append only; never reorder.
// Slate is the historical enumerator name for the theme presented as "Charcoal".
enum class ThemeId { Graphite, Slate, Nocturne };

// Every theme in menu order. Also the valid range for the persisted id.
inline constexpr ThemeId kAllThemes[] = {ThemeId::Graphite, ThemeId::Slate, ThemeId::Nocturne};

// The full color vocabulary a theme is built from. Everything in the QSS and the
// canvas pasteboard derives from these twelve roles.
//
// `border` and `outline` are deliberately distinct. A border is decorative chrome
// (a groove between panels) and stays subtle; an outline is the boundary that makes
// a control identifiable, so it is held at 3:1 against every surface it sits on.
// Conflating the two is why state was previously indistinguishable.
struct ThemeColors {
    QColor window;      // outermost app background
    QColor panel;       // dock / panel body
    QColor header;      // panel header, menu bar, tool strip, status bar
    QColor base;        // inset surfaces: lists, inputs
    QColor raised;      // hover / control fill
    QColor border;      // hairline separators (decorative; may be subtle)
    QColor outline;     // control boundaries (>= 3:1 on every surface)
    QColor text;        // primary text
    QColor textDim;     // secondary text, headers, disabled
    QColor accent;      // active tool, selection, focus (a line, not a flood)
    QColor accentText;  // text/icon on an accent fill (dark; the accent is light)
    QColor canvas;      // the pasteboard behind the document
};

[[nodiscard]] const ThemeColors& themeColors(ThemeId id) noexcept;

// The user-facing name. This is the single source of truth for the label, so the
// View menu builds itself from it rather than carrying a parallel list.
[[nodiscard]] const char* themeName(ThemeId id) noexcept;

// Fold a persisted integer to a valid theme, defaulting anything unknown. Keeps the
// whitelist in one place instead of at each QSettings read site.
[[nodiscard]] ThemeId themeFromInt(int value) noexcept;

// The theme currently applied (process-wide), so widgets like CanvasView can read
// the pasteboard color without threading it through every constructor.
[[nodiscard]] ThemeId currentTheme() noexcept;

// Build the application-wide QSS for a palette.
[[nodiscard]] QString buildStyleSheet(const ThemeColors& c);

// Apply `id` to the whole application: Fusion style, QPalette, app font, and QSS.
// Records it as the current theme.
void applyTheme(QApplication& app, ThemeId id);

}  // namespace pe::app
