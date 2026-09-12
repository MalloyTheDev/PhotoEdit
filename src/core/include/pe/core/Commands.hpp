#pragma once

#include "pe/core/BlendMode.hpp"
#include "pe/core/ColorProfile.hpp"
#include "pe/core/Command.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/Mask.hpp"    // MaskBuffer, stored by value in the Image Size snapshot
#include "pe/core/Orient.hpp"  // Orient (Image Rotation)
#include "pe/core/Selection.hpp"
#include "pe/core/TextLayer.hpp"  // TextModel, stored by value in the Image Size snapshot

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pe {

class Document;
class PaintCommand;

// --- Structural commands (operate on the top-level stack in M1) ---

// Insert a layer at a top-level index. Owns the layer while undone.
class AddLayerCommand final : public Command {
public:
    AddLayerCommand(std::unique_ptr<Layer> layer, std::size_t index);
    [[nodiscard]] std::string name() const override { return "Add Layer"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    std::unique_ptr<Layer> owned_;  // non-null only while the layer is out of the doc
    std::size_t index_;
    LayerId layerId_ = kNoLayer;
};

// Remove a top-level layer; restores it (and its position) on undo.
class RemoveLayerCommand final : public Command {
public:
    explicit RemoveLayerCommand(LayerId id);
    [[nodiscard]] std::string name() const override { return "Delete Layer"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId layerId_;
    std::unique_ptr<Layer> owned_;  // holds the removed layer while undone
    std::size_t index_ = 0;
    LayerId prevActive_ = kNoLayer;  // active layer before removal, restored on undo
    bool clearedActive_ = false;     // whether the active layer was inside the removed subtree
};

// Duplicate a top-level layer, inserting the copy directly above the original.
class DuplicateLayerCommand final : public Command {
public:
    explicit DuplicateLayerCommand(LayerId sourceId);
    [[nodiscard]] std::string name() const override { return "Duplicate Layer"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId sourceId_;
    std::unique_ptr<Layer> owned_;  // the clone while undone
    LayerId cloneId_ = kNoLayer;
    std::size_t index_ = 0;
};

// Flatten a set of TOP-LEVEL layers into one pixel layer, in place.
//
// `indices` are top-level indices, ascending; the merged layer takes the position of the
// lowest of them and every other one is removed. Not necessarily contiguous, because Merge
// Visible skips the hidden layers between the visible ones, which is what Photoshop does: the
// result lands where the bottom-most merged layer was, and anything skipped stays put.
//
// The merged pixels are the ordinary composite of exactly those layers, so opacity, blend
// mode, masks, clipping and adjustment layers are all baked in by the same code that draws
// them, rather than by a second implementation that could disagree with the canvas. The
// survivor is a plain pixel layer at full opacity in Normal, because everything that made it
// look the way it does is now in its pixels.
//
// Groups merge too: compositing one flattens it, which is what merging a group means.
//
// Refuses (execute is a no-op returning an unchanged document) when fewer than two layers are
// named, when an index is out of range, when the canvas is over the composite cap, or when the
// LOWEST named layer is clipped - its clipping base sits below the set, so merging would
// change the picture rather than preserve it.
class MergeLayersCommand final : public Command {
public:
    MergeLayersCommand(std::vector<std::size_t> indices, std::string name,
                       std::string mergedLayerName);
    ~MergeLayersCommand() override;
    [[nodiscard]] std::string name() const override { return name_; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

    // Whether the last execute() actually merged. A refused merge leaves the document alone;
    // the caller checks this to report a refusal rather than pushing a command that did
    // nothing.
    [[nodiscard]] bool merged() const noexcept { return mergedId_ != kNoLayer; }

    // Whole detached layers, held for undo: exactly what Command::retainedBytes exists for.
    // A merge of a full-canvas stack is hundreds of megabytes, and reporting zero would let
    // History carry it while believing the stack was empty.
    [[nodiscard]] std::int64_t retainedBytes() const noexcept override;

private:
    std::vector<std::size_t> indices_;
    std::string name_;
    std::string mergedLayerName_;
    std::vector<std::unique_ptr<Layer>> removed_;  // the originals, ascending, while undone
    LayerId mergedId_ = kNoLayer;
    LayerId prevActive_ = kNoLayer;
};

// Why a merge of `indices` cannot run, or None when it can. The command asks this too, so a
// caller that checks first and a command that refuses cannot disagree about what is mergeable.
enum class MergeBlock : std::uint8_t {
    None = 0,
    TooFewLayers,      // fewer than two named, or an index that is not a top-level layer
    LowestIsClipped,   // its clipping base sits below the set, so merging would change the picture
    OverCompositeCap,  // the canvas cannot be flattened in one allocation
};
[[nodiscard]] MergeBlock mergeBlocker(const Document& doc, std::span<const std::size_t> indices);

// The top-level indices each merge mode would take, ascending. Empty when the mode cannot run:
// no layer below the active one for Down, fewer than two visible layers for Visible, fewer than
// two layers for Flatten. The caller turns an empty result into a refusal.
[[nodiscard]] std::vector<std::size_t> mergeDownIndices(const Document& doc, LayerId active);
[[nodiscard]] std::vector<std::size_t> mergeVisibleIndices(const Document& doc);
[[nodiscard]] std::vector<std::size_t> flattenIndices(const Document& doc);

// Move a top-level layer to a new top-level index.
class ReorderLayerCommand final : public Command {
public:
    ReorderLayerCommand(LayerId id, std::size_t newIndex);
    [[nodiscard]] std::string name() const override { return "Reorder Layer"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId layerId_;
    std::size_t newIndex_;
    std::size_t oldIndex_ = 0;
};

// Group a set of existing top-level layers into a NEW GroupLayer, inserted at the
// position of the topmost grouped layer with the layers as its children in their
// prior relative order; the new group becomes active. Undo restores every layer to
// its exact prior top-level position and removes the group. The actual Layer objects
// are moved (never re-created), so their LayerIds — and any references to them —
// survive the round-trip. v1 requires the ids be distinct top-level siblings; empty,
// duplicate, unknown, or nested ids make the command a safe no-op.
class GroupLayersCommand final : public Command {
public:
    explicit GroupLayersCommand(std::vector<LayerId> ids);
    [[nodiscard]] std::string name() const override { return "Group Layers"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    std::vector<LayerId> members_;         // member ids, sorted by original index (ascending)
    std::vector<std::size_t> oldIndices_;  // original top-level index of each member (ascending)
    std::size_t insertIndex_ = 0;          // where the group goes (topmost member's index)
    LayerId groupId_ = kNoLayer;           // the created group's id (stable across undo/redo)
    LayerId prevActive_ = kNoLayer;        // active layer before grouping, restored on undo
    // The empty group shell while undone (members spliced back to top level); reused on
    // redo so the group keeps the same id and properties. Null while the group is live.
    std::unique_ptr<Layer> ownedGroup_;
    bool validated_ = false;  // input checked on first execute
    bool noop_ = false;       // invalid/empty input: execute/undo do nothing
};

// Dissolve a GroupLayer: splice its children into the parent at the group's position
// (preserving their order) and remove the now-empty group. Undo reconstructs the group
// with the same id, the same children (moved back, ids intact) in order, and restores
// the active layer. v1 dissolves top-level groups; a non-group / unknown / nested id is
// a safe no-op.
class UngroupCommand final : public Command {
public:
    explicit UngroupCommand(LayerId groupId);
    [[nodiscard]] std::string name() const override { return "Ungroup Layers"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId groupId_;                // the group to dissolve
    std::vector<LayerId> childIds_;  // its children, top-to-bottom, captured on first execute
    std::size_t groupIndex_ = 0;     // the group's top-level index (children land here)
    LayerId prevActive_ = kNoLayer;  // active layer before ungrouping, restored on undo
    // The emptied group shell while dissolved (children spliced to top level); reused on undo
    // so the rebuilt group keeps the same id and properties. Null while the group is live.
    std::unique_ptr<Layer> ownedGroup_;
    bool validated_ = false;  // input checked on first execute
    bool noop_ = false;       // non-group / unknown id: execute/undo do nothing
};

// --- Property commands (work on any layer, including nested) ---

class SetVisibilityCommand final : public Command {
public:
    SetVisibilityCommand(LayerId id, bool visible);
    [[nodiscard]] std::string name() const override { return "Toggle Visibility"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId layerId_;
    bool newVisible_;
    bool oldVisible_ = true;
};

class SetOpacityCommand final : public Command {
public:
    SetOpacityCommand(LayerId id, float opacity);
    [[nodiscard]] std::string name() const override { return "Change Opacity"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId layerId_;
    float newOpacity_;
    float oldOpacity_ = 1.0f;
};

// Fill opacity scales the layer's own pixels, and multiplies with opacity rather than
// replacing it, so the two are independent controls. Both are persisted from v8 on.
class SetFillOpacityCommand final : public Command {
public:
    SetFillOpacityCommand(LayerId id, float fillOpacity);
    [[nodiscard]] std::string name() const override { return "Change Fill Opacity"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId layerId_;
    float newFill_;
    float oldFill_ = 1.0f;
};

// Confine the layer to the coverage of the first non-clipped layer beneath it in the same
// stack, which is what "clip to the layer below" means. Nothing to clip to leaves the layer
// unclipped rather than hiding it.
class SetClippedCommand final : public Command {
public:
    SetClippedCommand(LayerId id, bool clipped);
    [[nodiscard]] std::string name() const override {
        return newClipped_ ? "Create Clipping Mask" : "Release Clipping Mask";
    }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId layerId_;
    bool newClipped_;
    bool oldClipped_ = false;
};

class SetBlendModeCommand final : public Command {
public:
    SetBlendModeCommand(LayerId id, BlendMode mode);
    [[nodiscard]] std::string name() const override { return "Change Blend Mode"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId layerId_;
    BlendMode newMode_;
    BlendMode oldMode_ = BlendMode::Normal;
};

class RenameLayerCommand final : public Command {
public:
    RenameLayerCommand(LayerId id, std::string name);
    [[nodiscard]] std::string name() const override { return "Rename Layer"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId layerId_;
    std::string newName_;
    std::string oldName_;
};

// --- Layer mask commands ---

// Attach a raster mask to a layer. RevealAll = an empty (fully revealing) mask; HideAll = a fully
// hidden mask over the canvas; FromSelection = reveal where the document's selection is set (an
// inactive selection reveals all). No-op if the layer is missing or already has a mask. Reversible:
// undo removes the mask, redo re-attaches the SAME mask object (built once on first execute), so
// any content later painted into it survives the round-trip.
class AddLayerMaskCommand final : public Command {
public:
    enum class Init { RevealAll, HideAll, FromSelection };
    AddLayerMaskCommand(LayerId id, Init init);
    [[nodiscard]] std::string name() const override { return "Add Layer Mask"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId layerId_;
    Init init_;
    std::unique_ptr<Mask> owned_;  // holds the mask while detached (undo); reused on redo
    bool validated_ = false;       // input checked + mask built on first execute
    bool noop_ = false;            // layer missing or already masked
};

// Remove a layer's mask. No-op if there is none. Reversible (undo restores the removed mask).
class RemoveLayerMaskCommand final : public Command {
public:
    explicit RemoveLayerMaskCommand(LayerId id);
    [[nodiscard]] std::string name() const override { return "Remove Layer Mask"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId layerId_;
    std::unique_ptr<Mask> owned_;  // the removed mask, restored on undo
    bool validated_ = false;
    bool noop_ = false;  // no mask to remove
};

// Enable/disable a layer's mask (the compositor ignores a disabled mask). Reversible; no-op if the
// layer has no mask.
class SetMaskEnabledCommand final : public Command {
public:
    SetMaskEnabledCommand(LayerId id, bool enabled);
    [[nodiscard]] std::string name() const override { return "Toggle Layer Mask"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    LayerId layerId_;
    bool newEnabled_;
    bool oldEnabled_ = true;
    bool validated_ = false;
    bool noop_ = false;  // no mask
};

// --- Color management ---

// Assign a color profile to the document: reinterpret its numbers under a new
// profile (the pixel values are unchanged; the appearance changes). Reversible.
// Pass a null profile to untag. (Convert, which transforms the pixels to preserve
// appearance, is a separate command.)
class AssignProfileCommand final : public Command {
public:
    explicit AssignProfileCommand(ColorProfileRef profile);
    [[nodiscard]] std::string name() const override { return "Assign Profile"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    ColorProfileRef newProfile_;
    ColorProfileRef oldProfile_;
    bool captured_ = false;  // oldProfile_ is filled on first execute
};

// Undoable selection change (marquee, Select All/Deselect/Invert, …). Snapshots the
// whole Selection by value on both sides, so it is exact at any canvas size (no
// fixed-bounds mask) and round-trips precisely through undo/redo.
class SetSelectionCommand final : public Command {
public:
    explicit SetSelectionCommand(Selection target);
    [[nodiscard]] std::string name() const override { return "Change Selection"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

    // Two whole selection masks. Selection stores its tiles BY VALUE, so nothing here is
    // shared with another command and both really are resident: at the tile cap that is
    // 268 MB per snapshot, which is why a step count alone could never bound this.
    [[nodiscard]] std::int64_t retainedBytes() const noexcept override {
        const auto perTile = static_cast<std::int64_t>(kTilePixels);
        return static_cast<std::int64_t>(newSel_.tileCount() + oldSel_.tileCount()) * perTile;
    }

private:
    Selection newSel_;  // the selection to apply
    Selection oldSel_;  // captured on first execute, for undo
    bool captured_ = false;
};

// Crop the document to a document-space rectangle: the canvas shrinks to the rect's size and
// every top-level pixel layer's content is shifted by -rect.topLeft, so the cropped region's
// top-left becomes the new origin. The active selection is shifted by the same -rect.topLeft so
// it tracks the cropped content. One undoable step (canvas size + per-layer content shifts +
// selection reverse together). Built on moveLayerContent; the rect is clamped to the canvas on
// execute.
// Where the existing content sits inside a resized canvas. The 3x3 grid every editor offers:
// the content keeps its pixel size and the new space is added (or removed) around it.
enum class CanvasAnchor : std::uint8_t {
    TopLeft = 0,
    Top,
    TopRight,
    Left,
    Center,
    Right,
    BottomLeft,
    Bottom,
    BottomRight,
};

// The shared machinery behind Crop and Canvas Size.
//
// Both do the same two things: change the canvas rectangle, and shift every piece of
// document-space geometry by one offset so the picture stays where it was relative to the
// content. That is more than the pixels: layer masks, text raster origins, fill-layer bounds
// and the selection all live in document space and all have to move together, and a resize
// that moved some of them is worse than one that refused.
//
// Derived classes differ only in how they work out the new size and the offset. Everything
// else - the all-or-nothing pre-flight, the per-layer content moves, the geometry sweep, the
// selection shift, and undoing all of it in the right order - lives here once.
class ReframeCommand : public Command {
public:
    ~ReframeCommand() override;  // out-of-line: the move vector holds incomplete PaintCommand
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

    // Whole content moves plus a selection snapshot, held for undo. Crop used to report the
    // base class's zero, which let History carry hundreds of megabytes believing its stacks
    // were empty.
    [[nodiscard]] std::int64_t retainedBytes() const noexcept override;

    // Whether the last execute() actually reframed. False when the plan was degenerate or the
    // content could not be shifted within the move budget, in which case the document is
    // untouched.
    [[nodiscard]] bool reframed() const noexcept { return !noop_; }

protected:
    // Work out the new canvas size and the offset to move content by, from the document as it
    // stands. Called once, on the first execute. Return false to make the command a no-op.
    [[nodiscard]] virtual bool plan(const Document& doc, Size& newSize, int& dx, int& dy) = 0;

private:
    void shiftGeometry(Document& doc, int dx, int dy);

    bool captured_ = false;
    bool noop_ = true;
    Size newSize_{};
    Size oldSize_{};
    int dx_ = 0;
    int dy_ = 0;
    Selection oldSel_;                                  // pre-reframe selection, for undo
    std::vector<std::unique_ptr<PaintCommand>> moves_;  // per-pixel-layer content shifts
    std::vector<LayerId> maskLayers_;                   // layers whose mask buffer shifted
    std::vector<LayerId> textLayers_;                   // text layers whose origin shifted
    std::vector<LayerId> fillLayers_;                   // fill layers whose bounds shifted
};

// Crop to `cropRect` (document space), clamped to the current canvas. The canvas becomes the
// rect's size and the content shifts by -rect.origin, so the cropped region lands at 0,0.
class CropCommand final : public ReframeCommand {
public:
    explicit CropCommand(Rect cropRect);
    [[nodiscard]] std::string name() const override { return "Crop"; }

protected:
    [[nodiscard]] bool plan(const Document& doc, Size& newSize, int& dx, int& dy) override;

private:
    Rect crop_;
};

// Change the canvas to `newSize`, keeping the content at its pixel size and positioning it by
// `anchor`. Unlike Crop this can GROW the canvas, and shrinking does not destroy anything: the
// tile store is sparse and unbounded, so content pushed outside the canvas is still there,
// still editable, and still written to .pedoc.
class ResizeCanvasCommand final : public ReframeCommand {
public:
    ResizeCanvasCommand(Size newSize, CanvasAnchor anchor);
    [[nodiscard]] std::string name() const override { return "Canvas Size"; }

protected:
    [[nodiscard]] bool plan(const Document& doc, Size& newSize, int& dx, int& dy) override;

private:
    Size target_;
    CanvasAnchor anchor_;
};

// The offset an anchor puts the old content at inside a new canvas. Exposed because it is the
// whole of what the 3x3 grid means, and it is worth being able to assert on directly.
[[nodiscard]] Point canvasAnchorOffset(Size oldSize, Size newSize, CanvasAnchor anchor) noexcept;

// Why a canvas resize cannot run, or None when it can.
enum class CanvasResizeBlock : std::uint8_t {
    None = 0,
    Unchanged,            // the size asked for is the size it already is
    DimensionOutOfRange,  // below 1 or past kMaxCanvasDimension on a side
    ContentNotShiftable,  // an anchor offset the move budget will not carry
};
[[nodiscard]] CanvasResizeBlock canvasResizeBlocker(const Document& doc, Size newSize,
                                                    CanvasAnchor anchor);

// ----------------------------------------------------------------- Image Size (resample)

// Why an Image Size resample cannot run, or None when it can. Mirrors CanvasResizeBlock, but the
// blocking condition is content that cannot be resampled within budget (a huge pixel layer, or a
// text raster that would scale past its round-trip cap) rather than content that cannot be shifted.
enum class ImageResizeBlock : std::uint8_t {
    None = 0,
    Unchanged,            // the size asked for is the size it already is
    DimensionOutOfRange,  // below 1 or past kMaxCanvasDimension on a side
    ContentTooLarge,      // a pixel layer over the resample budget, or a text raster over its cap
};
[[nodiscard]] ImageResizeBlock imageResizeBlocker(const Document& doc, Size newSize);

// Scale the whole document to `newSize`: every pixel layer, layer mask, text raster (and the
// re-rasterization hints), fill bounds and the selection resample together about the document
// origin, and the canvas becomes newSize. The engine behind Image Size.
//
// A SIBLING of ReframeCommand, deliberately not a subclass. Reframe translates every placement by
// one integer offset, which is exactly invertible, so its undo re-applies the opposite offset. A
// resample SCALES, which is lossy (a downscale discards detail a later upscale cannot recover), so
// undo cannot re-apply an inverse: it restores snapshots taken before the resample. The pixel
// deltas already carry their before-tiles (PaintCommand); the mask buffers, text content triples
// and fill bounds are snapshotted here, and the selection reuses the pre-resample copy.
//
// All-or-nothing: if any content-bearing layer cannot be resampled within budget (checked up front
// by imageResizeBlocker), nothing is applied and the command is a no-op, so the document is never
// left resized-but-not-resampled.
class ResampleDocumentCommand final : public Command {
public:
    explicit ResampleDocumentCommand(Size newSize);
    ~ResampleDocumentCommand() override;  // out-of-line: pixelMoves_ holds incomplete PaintCommand
    [[nodiscard]] std::string name() const override { return "Image Size"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

    // The resampled pixels plus the mask/text/fill and selection snapshots, all resident and held
    // for undo. Like ReframeCommand, this must not report the base class's zero, or History
    // miscounts its stacks and can carry hundreds of megabytes believing they are empty.
    [[nodiscard]] std::int64_t retainedBytes() const noexcept override;

    // Whether the last execute() actually resampled. False when the plan was degenerate or blocked,
    // in which case the document is untouched.
    [[nodiscard]] bool resampled() const noexcept { return !noop_; }

private:
    void applyScale(Document& doc);  // install the scaled content, canvas and selection

    struct TextSnapshot {
        LayerId id;
        TextModel model;
        PixelBuffer raster;
        Point origin;
    };

    Size target_{};
    bool captured_ = false;
    bool noop_ = true;
    Size oldSize_{};
    Selection oldSel_;                                       // pre-resample selection, for undo
    std::vector<std::unique_ptr<PaintCommand>> pixelMoves_;  // per-pixel-layer resample deltas
    std::vector<std::pair<LayerId, MaskBuffer>> oldMasks_;   // pre-resample mask buffers
    std::vector<TextSnapshot> oldText_;                      // pre-resample text content triples
    std::vector<std::pair<LayerId, Rect>> oldFills_;         // pre-resample fill bounds
};

// ----------------------------------------------------------------- Image Rotation (orient)

// Why an Image Rotation cannot run, or None when it can. A flip/rotate never yields an invalid
// canvas size and is never a no-op, so the only blocker is content too large to reorient in one
// step.
enum class OrientBlock : std::uint8_t {
    None = 0,
    ContentTooLarge,  // a pixel layer whose reorientation exceeds the move budget / coordinate
                      // range
};
[[nodiscard]] OrientBlock orientBlocker(const Document& doc, Orient op);

// Reorient the whole document by `op` (a flip or a 90/180 rotation): every pixel layer (recursing
// groups), layer mask, text raster, fill bounds and the selection turn together, and the canvas
// swaps sides on a quarter turn. As one undoable step.
//
// The exact, lossless cousin of ResampleDocumentCommand, and like it a SIBLING of ReframeCommand.
// A reorientation is a permutation, so it could in principle be undone by applying its inverse; it
// snapshots and restores instead, for one uniform undo contract with the resample (the pixel deltas
// already carry their before-tiles; the masks, text triples, fill bounds and selection are
// snapshotted here). All-or-nothing: if any content-bearing layer cannot be reoriented within
// budget, nothing is applied and the command is a no-op.
class OrientDocumentCommand final : public Command {
public:
    explicit OrientDocumentCommand(Orient op);
    ~OrientDocumentCommand() override;
    [[nodiscard]] std::string name() const override;
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;
    [[nodiscard]] std::int64_t retainedBytes() const noexcept override;

    // Whether the last execute() actually reoriented. False when blocked (over budget).
    [[nodiscard]] bool reoriented() const noexcept { return !noop_; }

private:
    void applyOrient(Document& doc);

    struct TextSnapshot {
        LayerId id;
        TextModel model;
        PixelBuffer raster;
        Point origin;
    };

    Orient op_;
    bool captured_ = false;
    bool noop_ = true;
    Size oldSize_{};
    Selection oldSel_;                                       // pre-rotation selection, for undo
    std::vector<std::unique_ptr<PaintCommand>> pixelMoves_;  // per-pixel-layer reorientations
    std::vector<std::pair<LayerId, MaskBuffer>> oldMasks_;   // pre-rotation mask buffers
    std::vector<TextSnapshot> oldText_;                      // pre-rotation text content triples
    std::vector<std::pair<LayerId, Rect>> oldFills_;         // pre-rotation fill bounds
};

// ----------------------------------------------------------------- Image Mode (bit depth)

// Why a bit-depth conversion cannot run, or None when it can.
enum class BitDepthBlock : std::uint8_t {
    None = 0,
    Unchanged,        // the document is already at the target depth
    ContentTooLarge,  // a pixel layer's converted store would exceed the move budget
};
[[nodiscard]] BitDepthBlock bitDepthBlocker(const Document& doc, BitDepth target);

// Convert the whole document to `target` bits per channel (Image > Mode): every pixel layer's tile
// store is rebuilt at the new depth, and the document tag is updated, as one undoable step.
//
// Widening (U8 -> U16/F32, U16 -> F32) is lossless; narrowing loses precision, so undo cannot
// reconstruct the old pixels -- it restores a snapshot taken before the conversion (a clone of each
// pixel layer, whose copy-on-write tiles keep the old pixels resident once the live store is
// rebuilt). Masks (8-bit), text rasters and fill colours are unaffected: only pixel-layer stores
// carry a depth.
class SetBitDepthCommand final : public Command {
public:
    explicit SetBitDepthCommand(BitDepth target);
    ~SetBitDepthCommand() override;
    [[nodiscard]] std::string name() const override;
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;
    [[nodiscard]] std::int64_t retainedBytes() const noexcept override;

    // Whether the last execute() actually converted. False when the plan was a no-op or blocked.
    [[nodiscard]] bool converted() const noexcept { return !noop_; }

private:
    void applyDepth(Document& doc);

    BitDepth target_;
    bool captured_ = false;
    bool noop_ = true;
    BitDepth oldDepth_ = BitDepth::U8;
    std::vector<std::pair<LayerId, std::unique_ptr<Layer>>> snapshots_;  // pre-conversion clones
};

}  // namespace pe
