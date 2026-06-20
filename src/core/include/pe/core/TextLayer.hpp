#pragma once

#include "pe/core/Command.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/PixelBuffer.hpp"

#include <string>
#include <utility>

namespace pe {

class Document;

// Round-trip contract for a TextLayer's cached raster. Producers (the app's text rasterizer) MUST
// keep the raster within these bounds, and the .pedoc reader rejects anything larger, so a saved
// document always reopens. Keeping the origin and dimensions within these caps also keeps
// contentBounds()'s Rect math (right()/bottom() == origin + extent) comfortably inside int.
inline constexpr int kMaxTextRasterDim = 8192;  // max raster width / height per side
inline constexpr std::int64_t kMaxTextRasterPixels = 16'000'000;  // max raster width * height

// The editable parameters of a TextLayer. Qt-FREE on purpose: font rasterization lives entirely in
// the app (src/app/TextRender, which uses QFont/QPainter), which turns this model into the cached
// raster the engine composites. The engine never measures or renders glyphs — it only stores this
// model (so the text can be re-edited and serialized) and the bytes the app produced.
struct TextModel {
    std::string text;           // the string (UTF-8)
    std::string fontFamily;     // e.g. "Arial"
    int pixelSize = 48;         // font size in px
    bool bold = false;          // synthetic/selected bold
    bool italic = false;        // synthetic/selected italic
    Rgba8 color{0, 0, 0, 255};  // fill color
    Point origin{0, 0};         // the doc-space point the user clicked (pre ink-offset placement)

    bool operator==(const TextModel&) const = default;
};

// A non-destructive text layer: an editable TextModel plus an app-supplied cached raster
// (straight-alpha RGBA8) placed so its (0,0) lands at rasterOrigin in document space. The engine
// treats rasterOrigin as opaque placement; the app's rasterizer decides the relationship to
// model.origin (today it bakes the glyph ink offset into the raster and sets rasterOrigin ==
// model.origin, but the two are stored independently in .pedoc so a future producer may differ).
// The compositor blits the cached raster per tile exactly like SolidColorLayer blits a flat color,
// so text gets blend mode / opacity / mask / clipping for free and is composited every frame rather
// than baked into pixels. Re-editing swaps in a fresh {model, raster, origin} as one undo step
// (EditTextCommand). The engine cannot regenerate the raster (no fonts), so the raster is the
// source of truth for appearance and round-trips through .pedoc verbatim; the model only drives the
// app's re-rasterize on edit.
class TextLayer final : public Layer {
public:
    TextLayer(TextModel model, PixelBuffer raster, Point rasterOrigin, std::string name = "Text");

    [[nodiscard]] const TextModel& model() const noexcept { return model_; }
    [[nodiscard]] const PixelBuffer& raster() const noexcept { return raster_; }
    [[nodiscard]] Point rasterOrigin() const noexcept { return rasterOrigin_; }

    // Atomically swap the whole content triple with the caller's (the undoable EditTextCommand
    // uses this so execute/undo are a single cheap swap).
    void swapContents(TextModel& model, PixelBuffer& raster, Point& rasterOrigin) noexcept {
        std::swap(model_, model);
        std::swap(raster_, raster);
        std::swap(rasterOrigin_, rasterOrigin);
    }

    [[nodiscard]] Rect contentBounds() const noexcept override {
        return raster_.isEmpty()
                   ? Rect{}
                   : Rect{rasterOrigin_.x, rasterOrigin_.y, raster_.width(), raster_.height()};
    }
    void renderInto(TileCoord coord, std::span<Rgbaf> dst) const override;
    [[nodiscard]] std::unique_ptr<Layer> clone() const override;

private:
    TextModel model_;
    PixelBuffer raster_;        // cached straight-alpha RGBA8 glyph raster (app-produced)
    Point rasterOrigin_{0, 0};  // doc-space placement of the raster's (0,0)
};

// Reversible edit of a text layer's content: swaps in a new {model, raster, origin} and restores
// the old on undo (one undo step). Reports Kind::Pixels over the UNION of the old and new bounds so
// both the vacated and the new glyph areas recomposite (the raster can move or resize on edit).
class EditTextCommand final : public Command {
public:
    EditTextCommand(LayerId layer, TextModel model, PixelBuffer raster, Point rasterOrigin);
    [[nodiscard]] std::string name() const override { return "Edit Text"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    DocumentChange swap(Document&);
    LayerId layer_;
    TextModel pending_;
    PixelBuffer pendingRaster_;
    Point pendingOrigin_;
};

}  // namespace pe
