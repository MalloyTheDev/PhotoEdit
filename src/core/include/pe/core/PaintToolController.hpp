#pragma once

#include "pe/core/Brush.hpp"
#include "pe/core/Color.hpp"
#include "pe/core/Layer.hpp"

#include <memory>
#include <vector>

namespace pe {

class Document;
class Selection;
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
    enum class Mode { Brush, Eraser };

    PaintToolController() = default;
    // Out-of-line so unique_ptr<PaintCommand> only needs a forward declaration here.
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

    [[nodiscard]] bool isStroking() const noexcept { return stroking_; }

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
    [[nodiscard]] std::unique_ptr<PaintCommand> buildStroke(Document& doc) const;

    BrushSettings brush_{};
    Rgbaf color_{0.0f, 0.0f, 0.0f, 1.0f};  // opaque black foreground default
    Mode mode_ = Mode::Brush;

    bool stroking_ = false;
    LayerId layer_ = kNoLayer;
    const Selection* selection_ = nullptr;
    std::vector<StrokePoint> points_;
    std::unique_ptr<PaintCommand> preview_;  // applied-to-doc provisional stroke
};

}  // namespace pe
