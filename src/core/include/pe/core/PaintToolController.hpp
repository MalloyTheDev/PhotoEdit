#pragma once

#include "pe/core/Brush.hpp"
#include "pe/core/Color.hpp"
#include "pe/core/Geometry.hpp"
#include "pe/core/Layer.hpp"

#include <memory>
#include <vector>

namespace pe {

class Document;
class Selection;
class Command;
class PaintCommand;

// Drives an interactive brush/eraser stroke from pointer input into the engine's
// paint pipeline (Brush.hpp). It is headless and UI-agnostic: a Qt widget — or a
// test — feeds it document-space sample points, and it owns the in-progress stroke.
//
// While the stroke is live it shows a preview by applying a provisional
// PaintCommand straight to the document (and reverting it before each rebuild), so
// the canvas reflects the stroke as it is drawn. On end() it reverts that preview
// and pushes ONE PaintCommand through the document's History, so the whole stroke
// is a single undo step regardless of how many samples it took. Because the
// preview is built fresh from the original tiles each time, the committed command's
// before/after snapshots are always byte-exact (one stroke == one undo step).
//
// Lifetime: a stroke borrows the Document& passed to begin/extend/end and the
// Selection* given to begin for the stroke's duration; finish a stroke (end or
// cancel) before destroying either. See docs/systems/08-brush-engine.md and
// docs/systems/09-tool-system.md.
class PaintToolController {
public:
    // MaskPaint paints into the active layer's MASK (not its pixels): each dab blends the mask
    // toward the foreground color's luminance (black hides, white reveals), gated by the selection.
    enum class Mode { Brush, Eraser, Dodge, Burn, Clone, Blur, Sharpen, Heal, MaskPaint };

    PaintToolController() = default;
    // Out-of-line so unique_ptr<Command> only needs a forward declaration here.
    ~PaintToolController();

    PaintToolController(const PaintToolController&) = delete;
    PaintToolController& operator=(const PaintToolController&) = delete;

    // --- configuration (session state; applies to the next stroke) ---
    void setBrush(const BrushSettings& s) noexcept { brush_ = s; }
    [[nodiscard]] const BrushSettings& brush() const noexcept { return brush_; }
    [[nodiscard]] BrushSettings& brush() noexcept { return brush_; }
    void setColor(Rgbaf c) noexcept { color_ = c; }
    [[nodiscard]] Rgbaf color() const noexcept { return color_; }
    void setMode(Mode m) noexcept { mode_ = m; }
    [[nodiscard]] Mode mode() const noexcept { return mode_; }

    // Clone Stamp source anchor (document space), set by Alt-click. The Clone-mode stroke samples
    // from here, with the source->dest offset locked to the stroke's first point. Persists across
    // strokes until reset; a Clone stroke before a source is set deposits nothing.
    void setCloneSource(Point p) noexcept {
        cloneSource_ = p;
        cloneSourceValid_ = true;
    }
    [[nodiscard]] bool hasCloneSource() const noexcept { return cloneSourceValid_; }
    void clearCloneSource() noexcept { cloneSourceValid_ = false; }  // e.g. on a document swap

    [[nodiscard]] bool isStroking() const noexcept { return stroking_; }

    // Document-space bounds touched so far by the in-progress stroke's preview
    // (the union of every previewed dab's dirty region; tile-granular). A view can
    // recomposite just this region per sample instead of the whole canvas. Reset by
    // begin(); empty when no stroke is active or nothing has been painted yet.
    [[nodiscard]] Rect strokeDirtyBounds() const noexcept { return strokeDirty_; }

    // --- interactive stroke lifecycle (document-space, sub-pixel) ---
    // Begin a stroke on the document's active layer, gated by `selection` if it is
    // non-null and active. Returns false (and starts nothing) when already stroking
    // or the active layer is not a paintable pixel layer. A preview dab appears
    // immediately so a single click marks the canvas.
    bool begin(Document& doc, StrokePoint p, const Selection* selection = nullptr);
    // Add a sample and refresh the live preview. No-op if not stroking.
    void extend(Document& doc, StrokePoint p);
    // Commit the stroke as one PaintCommand pushed to history. Returns true if any
    // pixels were painted (a command was pushed). No-op returning false if not
    // stroking or the stroke deposited nothing.
    bool end(Document& doc);
    // Abort the in-progress stroke, reverting the live preview. Commits nothing.
    void cancel(Document& doc);

private:
    void clearPreview(Document& doc);    // revert + drop the provisional command
    void rebuildPreview(Document& doc);  // recompute from points_ and apply (store at S0)
    // Returns the base Command so a stroke can be either a PaintCommand (pixel modes) or a
    // MaskPaintCommand (MaskPaint mode); both derive from Command and the preview uses it
    // polymorphically (execute/undo).
    [[nodiscard]] std::unique_ptr<Command> buildStroke(Document& doc) const;
    // The incremental LiveStroke for the current mode, or nullptr for modes with no incremental
    // path (Blur/Sharpen/Heal/MaskPaint) or when stabilization is on — those use the batched
    // rebuild. The per-pixel ops (Brush/Eraser/Dodge/Burn/Clone) get a LiveStroke so a long drag
    // stays linear rather than re-rasterizing the whole stroke every sample.
    [[nodiscard]] std::unique_ptr<LiveStroke> createLive(Document& doc);
    // Clone source->dest offset locked to the stroke's first point (validated/clamped). False if no
    // source is set or the first point is non-finite. Shared by buildStroke and createLive.
    [[nodiscard]] bool cloneOffset(int& offX, int& offY) const;
    // Whether the live stroke's target layer (layer_) still resolves to a paintable pixel layer.
    // A LiveStroke borrows the layer's tile store by reference for the whole stroke; if a
    // concurrent command removed or replaced that layer between samples (a contract violation, but
    // one the batched rebuild path tolerated by re-resolving by id), dereferencing the store would
    // be a use-after-free. extend/end/cancel consult this and drop the live stroke instead.
    [[nodiscard]] bool liveTargetValid(Document& doc) const;
    // Clear all per-stroke state (shared by end/cancel/abandon).
    void resetStroke() noexcept;

    BrushSettings brush_{};
    Rgbaf color_{0.0f, 0.0f, 0.0f, 1.0f};  // opaque black foreground default
    Mode mode_ = Mode::Brush;

    Point cloneSource_{};            // Clone Stamp source anchor (doc space)
    bool cloneSourceValid_ = false;  // set once the user Alt-clicks a source

    bool stroking_ = false;
    LayerId layer_ = kNoLayer;
    const Selection* selection_ = nullptr;
    std::vector<StrokePoint> points_;
    std::unique_ptr<Command> preview_;  // applied-to-doc provisional stroke (batched-rebuild path)
    std::unique_ptr<LiveStroke>
        live_;            // incremental stroke (per-pixel ops); null on the batched path
    Rect strokeDirty_{};  // cumulative dirty bounds of the live preview (see accessor)
};

}  // namespace pe
