#include "TextRender.hpp"

#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QPainter>
#include <QRect>
#include <QString>
#include <Qt>

#include <cstddef>
#include <cstring>

namespace pe::app {

namespace {
// Bound the rasterized text so a huge font size or long string can't trigger an enormous QImage
// allocation. These mirror the engine's TextLayer raster round-trip contract so this producer can
// never emit a raster the .pedoc reader would reject (a file you could save but not reopen).
constexpr int kMaxTextDim = pe::kMaxTextRasterDim;
constexpr long long kMaxTextPixels = pe::kMaxTextRasterPixels;
}  // namespace

pe::PixelBuffer renderText(const QString& text, const QFont& font, const QColor& color) {
    if (text.isEmpty()) return {};

    // Size the buffer from the ink bounding rect (not the advance box), so italic/script glyphs
    // whose ink overhangs the advance — including a negative left/top side bearing — are captured
    // rather than clipped. A small margin absorbs antialiasing fringe on all four sides.
    constexpr int kMargin = 2;
    const QFontMetrics fm(font);
    const QRect br = fm.boundingRect(text);
    const int w = br.width() + 2 * kMargin;
    const int h = br.height() + 2 * kMargin;
    if (w <= 0 || h <= 0 || w > kMaxTextDim || h > kMaxTextDim ||
        static_cast<long long>(w) * static_cast<long long>(h) > kMaxTextPixels) {
        return {};  // empty (whitespace-only) or pathologically large -> refuse before allocating
    }

    // Format_RGBA8888 stores bytes as R,G,B,A — the exact layout of pe::Rgba8 — and is straight
    // (non-premultiplied) alpha, so the buffer copies back row-for-row with no channel shuffle.
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::TextAntialiasing, true);
        p.setFont(font);
        p.setPen(color);
        // Offset the baseline so the ink rect (which may start left of / above the origin) lands
        // at (kMargin, kMargin) inside the buffer.
        p.drawText(kMargin - br.left(), kMargin - br.top(), text);
    }

    pe::PixelBuffer out(w, h);
    for (int y = 0; y < h; ++y) {
        std::memcpy(out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w),
                    img.constScanLine(y), static_cast<std::size_t>(w) * 4);
    }
    return out;
}

pe::PixelBuffer rasterizeText(const pe::TextModel& model) {
    QFont font(QString::fromStdString(model.fontFamily));
    font.setPixelSize(model.pixelSize);
    font.setBold(model.bold);
    font.setItalic(model.italic);
    const QColor color(model.color.r, model.color.g, model.color.b, model.color.a);
    return renderText(QString::fromStdString(model.text), font, color);
}

}  // namespace pe::app
