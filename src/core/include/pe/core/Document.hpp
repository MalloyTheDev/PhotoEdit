#pragma once

#include "pe/core/ColorProfile.hpp"
#include "pe/core/DocumentChange.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/History.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelFormat.hpp"
#include "pe/core/Selection.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pe {

// ColorMode and BitDepth now live in PixelFormat.hpp (shared with layers).

// Largest canvas dimension we accept (matches common pro tooling). Guards against
// overflow and absurd allocations; pixels are never allocated eagerly regardless.
inline constexpr int kMaxCanvasDimension = 300000;

class DocumentObserver {
public:
    virtual ~DocumentObserver() = default;
    virtual void onDocumentChanged(const Document&, const DocumentChange&) = 0;
};

// The single source of truth for an editable image. All structural/pixel mutation
// flows through History (commands); read accessors are const. See
// docs/systems/01-document-system.md.
class Document {
public:
    // Create a blank document with one transparent base pixel layer (at the given
    // bit depth) and no tiles allocated. Returns nullptr for a degenerate size or
    // non-positive ppi. ColorMode beyond RGB is modeled but not yet acted on.
    [[nodiscard]] static std::unique_ptr<Document> createBlank(Size canvasSize,
                                                               ColorMode mode = ColorMode::RGB,
                                                               BitDepth depth = BitDepth::U8,
                                                               int resolutionPpi = 72);

    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // --- geometry & color ---
    [[nodiscard]] Size canvasSize() const noexcept { return canvasSize_; }
    [[nodiscard]] Rect canvasBounds() const noexcept {
        return Rect{0, 0, canvasSize_.width, canvasSize_.height};
    }
    [[nodiscard]] int resolutionPpi() const noexcept { return resolutionPpi_; }
    [[nodiscard]] ColorMode colorMode() const noexcept { return colorMode_; }
    [[nodiscard]] BitDepth bitDepth() const noexcept { return bitDepth_; }
    // The document's color profile (the meaning of its numbers). Null until one is
    // assigned (untagged); set via assign/convert commands. See systems/15.
    [[nodiscard]] const ColorProfileRef& colorProfile() const noexcept { return profile_; }

    // --- model (read-only; mutate via commands) ---
    [[nodiscard]] std::span<const std::unique_ptr<Layer>> topLevelLayers() const noexcept {
        return root_.children();
    }
    [[nodiscard]] std::size_t topLevelCount() const noexcept { return root_.childCount(); }
    // Recursive lookup across nested groups (const and mutable).
    [[nodiscard]] const Layer* findLayer(LayerId id) const noexcept;
    [[nodiscard]] Layer* findLayer(LayerId id) noexcept;

    // --- session state ---
    [[nodiscard]] LayerId activeLayer() const noexcept { return activeLayer_; }
    [[nodiscard]] bool isDirty() const noexcept { return dirty_; }

    // Selection (document-wide edit mask). Owned by the document. Inactive by
    // default (whole canvas editable). Tools mutate via the editable accessor
    // (session state, emits Selection change; undoable SelectCommands later).
    [[nodiscard]] const Selection& selection() const noexcept { return selection_; }
    [[nodiscard]] Selection& editableSelection() noexcept { return selection_; }

    // Call after directly mutating the selection via editableSelection() (e.g. from
    // a marquee tool gesture) so observers are notified for marching ants etc.
    void touchSelection();

    // --- snapshots ---

    // An immutable, point-in-time copy of this document, for a worker thread.
    //
    // WHAT IS CAPTURED: canvas size, color mode, bit depth, resolution, color profile,
    // the whole layer tree with each layer's properties, mask and pixels, and which
    // layer is active. Layer ids are preserved, so an id taken from this document
    // resolves in the snapshot to the same layer. That is exactly the set a save or an
    // export reads.
    //
    // WHAT IS NOT: history, the selection, observers, and the dirty flag. Nothing that
    // consumes a snapshot reads them, and Selection stores its mask tiles BY VALUE, so
    // copying one is a real cost (up to about 268 MB at the tile cap) rather than a
    // pointer bump. A snapshot's selection is therefore inactive, not a copy of this
    // one; do not add a consumer that reads it without capturing it here first.
    //
    // WHAT IS SHARED: the pixel tile buffers, which are reference-counted and
    // copy-on-write. Taking a snapshot marks every tile it captures shared, and the
    // live document forks a tile before its next in-place write, so the snapshot's
    // bytes can never change under a reader. Cost is therefore O(tile count) pointer
    // copies plus the layer metadata, NOT O(pixels): a 24 MP document snapshots in
    // about a millisecond. Layer MASKS are the exception and are deep-copied, because
    // MaskBuffer stores its tiles by value.
    //
    // THREADING: the snapshot is a separate object graph that nothing else references,
    // so a worker may read it freely while this document keeps being edited on the
    // owning thread. The one rule is that the snapshot must be created on, and the live
    // document must keep being mutated from, the same thread: the fork flag is set by
    // that thread before the worker exists. Do not create a snapshot on a worker.
    [[nodiscard]] std::unique_ptr<const Document> snapshot() const;

    // --- compositing convenience (headless preview / tests) ---
    [[nodiscard]] PixelBuffer compositeImage() const;

    // Flatten at higher precision (docs/systems/15). Even with today's 8-bit layer
    // storage these are worthwhile: compositing and adjustment layers run in float,
    // so emitting 16-bit/float avoids the final 8-bit quantization that bands a
    // heavily-adjusted stack. The document's bitDepth() tells a caller which to use.
    [[nodiscard]] PixelBuffer16 compositeImage16() const;
    [[nodiscard]] PixelBufferF compositeImageF() const;

    // --- mutation entry point & observers ---
    [[nodiscard]] History& history() noexcept { return history_; }
    // Read-only view, for a const document (a snapshot is const by construction).
    [[nodiscard]] const History& history() const noexcept { return history_; }
    void addObserver(DocumentObserver*);
    void removeObserver(DocumentObserver*);

    // ---- command-facing mutation surface ----
    // These are the methods commands use; callers other than commands should not
    // touch them (the discipline keeps undo/observers correct). Not undoable on
    // their own — a Command pairs the forward and inverse calls.
    void cmdInsertTopLevel(std::size_t index, std::unique_ptr<Layer> layer);
    [[nodiscard]] std::unique_ptr<Layer> cmdRemoveTopLevel(LayerId id);
    [[nodiscard]] std::size_t topLevelIndexOf(LayerId id) const noexcept;
    // Active layer is session state: emits a change but is not undoable.
    void setActiveLayer(LayerId id);
    // Replace the document's color profile (assign reinterprets; convert pairs this with a pixel
    // transform). A pure mutator — it does NOT notify (matching
    // cmdInsertTopLevel/cmdSetCanvasSize); the command returns the DocumentChange so History
    // notifies once (AssignProfileCommand returns Profile; ConvertProfileCommand returns Pixels,
    // whose recomposite covers the appearance change).
    void cmdSetColorProfile(ColorProfileRef profile);
    // Resize the canvas (the document's pixel extent). Clamped to [1, kMaxCanvasDimension]
    // per dimension. Layer content is not moved here — CropCommand pairs this with per-layer
    // content shifts; resolution/profile are unchanged. Not undoable on its own.
    void cmdSetCanvasSize(Size newSize) noexcept;
    // Set the document's bit depth tag. The per-layer store conversion is done by
    // SetBitDepthCommand, which pairs this with a convertDepth() on every pixel layer; this only
    // updates the document-wide tag. Not undoable on its own.
    void cmdSetBitDepth(BitDepth depth) noexcept;

private:
    friend class History;  // History calls notify()/setDirty() after a command.
    Document(Size canvasSize, ColorMode mode, BitDepth depth, int ppi);

    void notify(const DocumentChange&);
    void setDirty(bool dirty);

    Size canvasSize_{};
    int resolutionPpi_ = 72;
    ColorMode colorMode_ = ColorMode::RGB;
    BitDepth bitDepth_ = BitDepth::U8;
    ColorProfileRef profile_;  // null == untagged
    GroupLayer root_{"<root>"};
    LayerId activeLayer_ = kNoLayer;
    bool dirty_ = false;
    Selection selection_;
    History history_;
    std::vector<DocumentObserver*> observers_;
};

}  // namespace pe
