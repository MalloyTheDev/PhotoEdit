#pragma once

#include "pe/core/BlendMode.hpp"
#include "pe/core/Command.hpp"
#include "pe/core/Layer.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace pe {

class Document;

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

}  // namespace pe
