#include "pe/core/TextLayer.hpp"

#include "pe/core/Document.hpp"

#include <cstddef>
#include <utility>

namespace pe {

TextLayer::TextLayer(TextModel model, PixelBuffer raster, Point rasterOrigin, std::string name)
    : Layer(LayerKind::Text, std::move(name)),
      model_(std::move(model)),
      raster_(std::move(raster)),
      rasterOrigin_(rasterOrigin) {}

void TextLayer::renderInto(TileCoord coord, std::span<Rgbaf> dst) const {
    const Rect tile = tileBounds(coord);
    // Cull whole tiles that don't touch the raster's placement rect (transparent there).
    if (tile.intersected(contentBounds()).isEmpty()) {
        for (Rgbaf& p : dst) p = Rgbaf{};
        return;
    }
    // Mixed/covered tile: sample the cached raster per pixel, transparent outside it. Unlike a flat
    // SolidColorLayer there is no "entirely inside" constant fast path (the raster varies).
    const int baseX = coord.col * kTileSize;
    const int baseY = coord.row * kTileSize;
    for (int ly = 0; ly < kTileSize; ++ly) {
        const int ry = baseY + ly - rasterOrigin_.y;
        const bool yIn = ry >= 0 && ry < raster_.height();
        for (int lx = 0; lx < kTileSize; ++lx) {
            const int rx = baseX + lx - rasterOrigin_.x;
            const bool in = yIn && rx >= 0 && rx < raster_.width();
            dst[static_cast<std::size_t>(ly) * kTileSize + static_cast<std::size_t>(lx)] =
                in ? toFloat(raster_.at(rx, ry)) : Rgbaf{};
        }
    }
}

std::unique_ptr<Layer> TextLayer::clone() const {
    auto copy = std::make_unique<TextLayer>(model_, raster_, rasterOrigin_, name());
    copyPropsTo(*copy);  // universal props + a deep mask copy; raster/model copied by value above
    return copy;
}

// ---------------------------------------------------------------- EditText

EditTextCommand::EditTextCommand(LayerId layer, TextModel model, PixelBuffer raster,
                                 Point rasterOrigin)
    : layer_(layer),
      pending_(std::move(model)),
      pendingRaster_(std::move(raster)),
      pendingOrigin_(rasterOrigin) {}

DocumentChange EditTextCommand::swap(Document& doc) {
    Layer* layer = doc.findLayer(layer_);
    if (layer == nullptr || layer->kind() != LayerKind::Text) {
        return DocumentChange{DocumentChange::Kind::Pixels, Rect{}, layer_};
    }
    auto* text = static_cast<TextLayer*>(layer);
    const Rect before = text->contentBounds();
    text->swapContents(pending_, pendingRaster_, pendingOrigin_);
    const Rect after = text->contentBounds();
    // Union so the OLD glyph area (now vacated) and the NEW one both recomposite.
    return DocumentChange{DocumentChange::Kind::Pixels, before.united(after), layer_};
}

DocumentChange EditTextCommand::execute(Document& doc) {
    return swap(doc);
}
DocumentChange EditTextCommand::undo(Document& doc) {
    return swap(doc);
}

}  // namespace pe
