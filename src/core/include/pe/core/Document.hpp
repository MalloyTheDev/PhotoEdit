#pragma once

#include "pe/core/DocumentChange.hpp"
#include "pe/core/GroupLayer.hpp"
#include "pe/core/History.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/PixelBuffer.hpp"
#include "pe/core/PixelFormat.hpp"

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

private:
    friend class History;  // History calls notify()/setDirty() after a command.
    Document(Size canvasSize, ColorMode mode, BitDepth depth, int ppi);

    void notify(const DocumentChange&);
    void setDirty(bool dirty);

    Size canvasSize_{};
    int resolutionPpi_ = 72;
    ColorMode colorMode_ = ColorMode::RGB;
    BitDepth bitDepth_ = BitDepth::U8;
    GroupLayer root_{"<root>"};
    LayerId activeLayer_ = kNoLayer;
    bool dirty_ = false;
    History history_;
    std::vector<DocumentObserver*> observers_;
};

}  // namespace pe
