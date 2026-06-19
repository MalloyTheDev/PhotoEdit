#pragma once

#include "pe/core/BlendMode.hpp"
#include "pe/core/ColorProfile.hpp"
#include "pe/core/Command.hpp"
#include "pe/core/Layer.hpp"
#include "pe/core/Selection.hpp"

#include <cstddef>
#include <memory>
#include <string>
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
class CropCommand final : public Command {
public:
    explicit CropCommand(Rect cropRect);
    ~CropCommand() override;  // out-of-line: the move vector holds incomplete PaintCommand

    [[nodiscard]] std::string name() const override { return "Crop"; }
    DocumentChange execute(Document&) override;
    DocumentChange undo(Document&) override;

private:
    Rect crop_;
    Size oldSize_{};
    bool captured_ = false;
    std::vector<std::unique_ptr<PaintCommand>> moves_;  // per-layer content shift to the origin
    Selection oldSel_;  // selection before crop, captured on first execute, restored on undo
};

}  // namespace pe
