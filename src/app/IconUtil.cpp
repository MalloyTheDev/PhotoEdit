#include "IconUtil.hpp"

#include <QByteArray>
#include <QFile>
#include <QPainter>
#include <QRectF>
#include <QSvgRenderer>

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

QPixmap renderIcon(const QString& name, const QColor& color, int logical) {
    QByteArray data;
    QFile f(QStringLiteral(":/icons/%1.svg").arg(name));
    if (f.open(QIODevice::ReadOnly)) data = f.readAll();
    // The bundled glyphs ship a neutral light stroke ("#cfd3da"); recolor it.
    data.replace("#cfd3da", color.name().toUtf8());

    constexpr qreal kDpr = 2.0;
    QPixmap pm(static_cast<int>(logical * kDpr), static_cast<int>(logical * kDpr));
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(kDpr);
    QSvgRenderer renderer(data);
    QPainter p(&pm);
    renderer.render(&p, QRectF(0, 0, logical, logical));
    p.end();
    return pm;
}

QIcon renderIconAsIcon(const QString& name, const QColor& color, int logical) {
    return QIcon(renderIcon(name, color, logical));
}

}  // namespace pe::app
