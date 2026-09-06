// Theme regression tests for the Qt shell.
//
// These exist because the palette is load-bearing for accessibility: state in this
// UI is carried by an accent line and a control outline, and if either drops below
// its contrast bar the affordance silently disappears. The ratios were computed by
// hand when the palette landed; this pins them so a future tweak cannot quietly
// undo the work.
//
// Nothing here needs a QApplication: QColor and QString are enough, and the
// resource check uses QFile only.

#include "IconUtil.hpp"
#include "Theme.hpp"
#include "pe_test.hpp"

#include <QColor>
#include <QFile>
#include <QString>
#include <QStringList>

#include <cmath>
#include <iterator>
#include <set>
#include <string>

namespace {

// WCAG 2.1 relative luminance. Channel values are sRGB in [0,1].
double channelLuminance(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const QColor& c) {
    return 0.2126 * channelLuminance(c.redF()) + 0.7152 * channelLuminance(c.greenF()) +
           0.0722 * channelLuminance(c.blueF());
}

// WCAG 2.1 contrast ratio, always >= 1.0 regardless of argument order.
double contrast(const QColor& a, const QColor& b) {
    const double la = relativeLuminance(a);
    const double lb = relativeLuminance(b);
    const double hi = la > lb ? la : lb;
    const double lo = la > lb ? lb : la;
    return (hi + 0.05) / (lo + 0.05);
}

// Text must clear 4.5:1 (the theme pins the UI font at 12px, so the large-text
// exemption never applies). Non-text boundaries and state indicators clear 3:1.
constexpr double kTextRatio = 4.5;
constexpr double kUiRatio = 3.0;

}  // namespace

PE_TEST(theme_text_clears_wcag_on_every_surface) {
    for (const pe::app::ThemeId id : pe::app::kAllThemes) {
        const pe::app::ThemeColors& c = pe::app::themeColors(id);
        const QColor surfaces[] = {c.window, c.panel, c.header, c.base};
        for (const QColor& s : surfaces) {
            PE_CHECK(contrast(c.text, s) >= kTextRatio);
            PE_CHECK(contrast(c.textDim, s) >= kTextRatio);
        }
    }
}

PE_TEST(theme_accent_and_outline_clear_wcag_on_every_surface) {
    // The accent carries focus, the checked tool, the selected row and the active
    // tab; the outline is what makes a control identifiable. Both are non-text.
    for (const pe::app::ThemeId id : pe::app::kAllThemes) {
        const pe::app::ThemeColors& c = pe::app::themeColors(id);
        const QColor surfaces[] = {c.window, c.panel, c.header, c.base};
        for (const QColor& s : surfaces) {
            PE_CHECK(contrast(c.accent, s) >= kUiRatio);
            PE_CHECK(contrast(c.outline, s) >= kUiRatio);
        }
    }
}

PE_TEST(theme_accent_text_is_readable_on_the_accent_fill) {
    // A filled accent (selected menu item, pressed button) carries accentText.
    for (const pe::app::ThemeId id : pe::app::kAllThemes) {
        const pe::app::ThemeColors& c = pe::app::themeColors(id);
        PE_CHECK(contrast(c.accentText, c.accent) >= kTextRatio);
    }
}

PE_TEST(theme_accent_is_lighter_than_its_ground) {
    // The accent is used as a line and an outline, so it must read as a highlight
    // against the chrome rather than a hole in it. This also encodes why
    // accentText is dark: the fill underneath it is light.
    for (const pe::app::ThemeId id : pe::app::kAllThemes) {
        const pe::app::ThemeColors& c = pe::app::themeColors(id);
        PE_CHECK(relativeLuminance(c.accent) > relativeLuminance(c.panel));
        PE_CHECK(relativeLuminance(c.accentText) < relativeLuminance(c.accent));
    }
}

PE_TEST(theme_stylesheet_substitutes_every_token) {
    // buildStyleSheet replaces @name@ placeholders from a hand-maintained table.
    // Adding a role to ThemeColors without adding its row leaves a literal
    // "@role@" in the QSS, which Qt silently ignores: the rule just stops working.
    for (const pe::app::ThemeId id : pe::app::kAllThemes) {
        const QString qss = pe::app::buildStyleSheet(pe::app::themeColors(id));
        PE_CHECK(!qss.isEmpty());
        PE_CHECK(!qss.contains(QLatin1Char('@')));
    }
}

PE_TEST(theme_names_are_present_and_unique) {
    std::set<std::string> seen;
    for (const pe::app::ThemeId id : pe::app::kAllThemes) {
        const char* name = pe::app::themeName(id);
        PE_CHECK(name != nullptr && name[0] != '\0');
        PE_CHECK(seen.insert(std::string(name)).second);
    }
    // The View menu builds one entry per entry here, so this is also the menu size.
    PE_CHECK_EQ(seen.size(), std::size(pe::app::kAllThemes));
}

PE_TEST(theme_from_int_round_trips_and_folds_unknown_ids) {
    for (const pe::app::ThemeId id : pe::app::kAllThemes) {
        PE_CHECK(pe::app::themeFromInt(static_cast<int>(id)) == id);
    }
    // A persisted setting is attacker-adjacent only in the sense that it is
    // untrusted input: any out-of-range value must fold, never index blindly.
    const int bogus[] = {-1, -999, 3, 4, 1000};
    for (const int v : bogus) {
        const pe::app::ThemeId folded = pe::app::themeFromInt(v);
        bool valid = false;
        for (const pe::app::ThemeId id : pe::app::kAllThemes) valid = valid || folded == id;
        PE_CHECK(valid);
    }
}

PE_TEST(app_icon_resources_are_linked_into_the_library) {
    // pe_app is a static library, so the linker discards the generated resource
    // initializer unless a linked translation unit references it. main() calls this
    // for the same reason; if either call is lost the tool strip renders as blank
    // squares, which is easy to miss in a headless build.
    pe::app::initIconResources();

    const QStringList probes = {QStringLiteral(":/icons/paintbrush.svg"),
                                QStringLiteral(":/icons/eraser.svg"),
                                QStringLiteral(":/icons/move.svg")};
    for (const QString& path : probes) {
        QFile f(path);
        PE_CHECK(f.exists());
        if (f.open(QIODevice::ReadOnly)) {
            PE_CHECK(!f.readAll().isEmpty());
        } else {
            PE_CHECK(false);
        }
    }
}
