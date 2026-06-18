#include "TextRender.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QSize>
#include <QString>
#include <Qt>

#include <cstdint>

namespace pe::app {

namespace {
// Bound the rasterized text so a huge font size or long string can't trigger an enormous QImage
// allocation. 16 MP matches the engine's per-edit cap (stampBuffer would reject a larger region
// anyway); the per-dimension bound guards the multiply.
constexpr int kMaxTextDim = 8192;
constexpr long long kMaxTextPixels = 16'000'000;
}  // namespace

pe::PixelBuffer renderText(const QString& text, const QFont& font, const QColor& color) {
    if (text.isEmpty()) return {};

    const QFontMetrics fm(font);
    const QSize sz = fm.size(Qt::TextSingleLine, text);
    const int w = sz.width() + 4;  // small margins so AA / italic overhang isn't clipped
    const int h = sz.height() + 2;
    if (w <= 0 || h <= 0 || w > kMaxTextDim || h > kMaxTextDim ||
        static_cast<long long>(w) * static_cast<long long>(h) > kMaxTextPixels) {
        return {};  // empty or pathologically large -> refuse before allocating
    }

    QImage img(w, h, QImage::Format_ARGB32);  // straight (non-premultiplied) alpha, like Rgba8
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        p.setFont(font);
        p.setPen(color);
        p.drawText(2, fm.ascent(), text);  // 2px left margin; baseline within the buffer
    }

    pe::PixelBuffer out(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const QRgb c = img.pixel(x, y);
            out.set(x, y,
                    pe::Rgba8{
                        static_cast<std::uint8_t>(qRed(c)), static_cast<std::uint8_t>(qGreen(c)),
                        static_cast<std::uint8_t>(qBlue(c)), static_cast<std::uint8_t>(qAlpha(c))});
        }
    }
    return out;
}

}  // namespace pe::app
