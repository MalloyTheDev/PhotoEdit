#include "IconUtil.hpp"

#include "Theme.hpp"

#include <QByteArray>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QRectF>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>

// Q_INIT_RESOURCE expands to a call to a function rcc generates at global scope, so
// it cannot be invoked from inside a namespace. This shim is the documented way to
// keep the public entry point namespaced.
static void peAppInitIconResources() {
    Q_INIT_RESOURCE(icons);
}

namespace pe::app {

void initIconResources() {
    peAppInitIconResources();
}

QColor themeIconColor() {
    return themeColors(currentTheme()).text;
}

QPixmap renderIcon(const QString& name, const QColor& color, int logical, qreal dpr) {
    if (dpr <= 0.0) {
        // The display's real ratio rather than an assumed one. qApp can be absent in
        // a non-GUI context, so fall back to 1x rather than dividing by nothing.
        dpr = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
        if (dpr <= 0.0) dpr = 1.0;
    }

    QByteArray data;
    QFile f(QStringLiteral(":/icons/%1.svg").arg(name));
    if (f.open(QIODevice::ReadOnly)) data = f.readAll();
    // The bundled glyphs ship a neutral light stroke ("#cfd3da"); recolor it.
    data.replace("#cfd3da", color.name().toUtf8());

    const int physical = std::max(1, static_cast<int>(std::lround(logical * dpr)));
    QPixmap pm(physical, physical);
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(dpr);
    QSvgRenderer renderer(data);
    QPainter p(&pm);
    renderer.render(&p, QRectF(0, 0, logical, logical));
    p.end();
    return pm;
}

QIcon renderIconAsIcon(const QString& name, const QColor& color, int logical) {
    // Three rasterizations so Qt has a real choice per screen. A single fixed-size
    // pixmap cannot follow a window moved between displays of different scale.
    QIcon icon;
    for (const qreal dpr : {1.0, 2.0, 3.0}) {
        icon.addPixmap(renderIcon(name, color, logical, dpr));
    }
    return icon;
}

}  // namespace pe::app
