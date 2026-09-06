// Icon rasterization and tinting.
//
// Two defects motivated these. The device pixel ratio was hard-coded to 2.0, so
// glyphs were rasterized at the wrong resolution on every display that is not
// exactly 2x, and a single fixed pixmap could not follow a window moved to a
// display with a different scale. The tint was a hard-coded constant, so the tool
// strip kept one colour no matter which theme was active.

#include "IconUtil.hpp"
#include "Theme.hpp"
#include "pe_test.hpp"

#include <QApplication>
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QList>
#include <QPixmap>
#include <QSize>
#include <QString>

namespace {

// A glyph that is definitely bundled; the resource test elsewhere pins its presence.
const QString kGlyph = QStringLiteral("paintbrush");

// The strongest-alpha pixel's colour, which is the stroke the tint should have set.
QColor dominantStrokeColor(const QPixmap& pm) {
    const QImage img = pm.toImage();
    QColor best;
    int bestAlpha = 0;
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.alpha() > bestAlpha) {
                bestAlpha = c.alpha();
                best = c;
            }
        }
    }
    return best;
}

}  // namespace

PE_TEST(iconutil_honours_the_requested_device_pixel_ratio) {
    // Previously every icon came back at a fixed 2x regardless of the display.
    for (const qreal dpr : {1.0, 2.0, 3.0}) {
        const QPixmap pm = pe::app::renderIcon(kGlyph, QColor(255, 255, 255), 22, dpr);
        PE_CHECK(!pm.isNull());
        PE_CHECK_EQ(pm.width(), static_cast<int>(22 * dpr));
        PE_CHECK_EQ(pm.height(), static_cast<int>(22 * dpr));
        // The ratio has to be recorded on the pixmap too, or Qt draws it at the
        // wrong logical size.
        PE_CHECK(qFuzzyCompare(pm.devicePixelRatio(), dpr));
    }
}

PE_TEST(iconutil_icon_carries_several_resolutions) {
    // One pixmap cannot follow a window dragged between displays of different scale.
    const QIcon icon = pe::app::renderIconAsIcon(kGlyph, QColor(255, 255, 255), 22);
    PE_CHECK(!icon.isNull());
    const QList<QSize> sizes = icon.availableSizes();
    PE_CHECK(sizes.size() >= 3);

    // And they must be genuinely different sizes, not three copies of one.
    int distinct = 0;
    for (const QSize& a : sizes) {
        bool seenEarlier = false;
        for (const QSize& b : sizes) {
            if (&a == &b) break;
            if (a == b) seenEarlier = true;
        }
        if (!seenEarlier) ++distinct;
    }
    PE_CHECK(distinct >= 3);
}

PE_TEST(iconutil_tints_the_glyph_to_the_requested_colour) {
    // The recolour is a string replacement inside the SVG, so it is worth checking
    // that it actually reached the rasterized pixels rather than silently missing.
    const QPixmap red = pe::app::renderIcon(kGlyph, QColor(255, 0, 0), 32, 1.0);
    const QColor stroke = dominantStrokeColor(red);
    PE_CHECK(stroke.alpha() > 0);
    PE_CHECK(stroke.red() > 200);
    PE_CHECK(stroke.green() < 80);
    PE_CHECK(stroke.blue() < 80);

    const QPixmap green = pe::app::renderIcon(kGlyph, QColor(0, 255, 0), 32, 1.0);
    const QColor other = dominantStrokeColor(green);
    PE_CHECK(other.green() > 200);
    PE_CHECK(other.red() < 80);
}

PE_TEST(iconutil_tint_follows_the_active_theme) {
    // The tint used to be a constant, so switching theme left the glyphs behind.
    // Every theme must map to its own text colour.
    for (const pe::app::ThemeId id : pe::app::kAllThemes) {
        pe::app::applyTheme(*qApp, id);
        PE_CHECK_EQ(pe::app::themeIconColor(), pe::app::themeColors(id).text);
    }

    // And the themes must not all resolve to the same tint, or following the theme
    // would be indistinguishable from ignoring it.
    pe::app::applyTheme(*qApp, pe::app::ThemeId::Nocturne);
    const QColor nocturne = pe::app::themeIconColor();
    pe::app::applyTheme(*qApp, pe::app::ThemeId::Graphite);
    const QColor graphite = pe::app::themeIconColor();
    PE_CHECK(nocturne != graphite);

    pe::app::applyTheme(*qApp, pe::app::ThemeId::Nocturne);  // leave the default active
}
