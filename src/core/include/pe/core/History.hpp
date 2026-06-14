#pragma once

#include "pe/core/Command.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace pe {

class Document;

// The undo/redo stack. push() executes a command, notifies observers, marks the
// document dirty, and discards the redo stack. undo()/redo() walk the stacks.
// A "saved marker" tracks the last-saved point so the dirty flag clears when the
// user undoes back to it. See docs/systems/21-history-undo.md and ADR-0005.
class History {
public:
    explicit History(Document& doc) noexcept : doc_(&doc) {}

    History(const History&) = delete;
    History& operator=(const History&) = delete;

    // Execute and record a command. Truncates any redo history.
    void push(std::unique_ptr<Command> cmd);

    [[nodiscard]] bool canUndo() const noexcept { return !done_.empty(); }
    [[nodiscard]] bool canRedo() const noexcept { return !undone_.empty(); }
    void undo();
    void redo();

    [[nodiscard]] std::size_t undoDepth() const noexcept { return done_.size(); }
    [[nodiscard]] std::size_t redoDepth() const noexcept { return undone_.size(); }

    // Names for the History panel ("" if nothing to undo/redo).
    [[nodiscard]] std::string topUndoName() const;
    [[nodiscard]] std::string topRedoName() const;

    // Bound the number of retained undo steps (memory). Default 100.
    void setLimit(std::size_t n) noexcept { limit_ = n; }
    [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

    // Saved-state tracking: call after a successful save.
    void markSaved() noexcept;
    [[nodiscard]] bool isAtSavedState() const noexcept;

private:
    void trimToLimit();
    void updateDirty();

    Document* doc_;
    std::vector<std::unique_ptr<Command>> done_;
    std::vector<std::unique_ptr<Command>> undone_;
    std::size_t limit_ = 100;
    // done_.size() at the last save, or -1 if the saved state was trimmed away
    // (and thus can never be returned to → always dirty).
    std::ptrdiff_t savedDepth_ = 0;
};

}  // namespace pe
