#include "pe/core/History.hpp"

#include "pe/core/Document.hpp"

namespace pe {

void History::push(std::unique_ptr<Command> cmd) {
    if (!cmd) return;
    // If the saved point lies on the redo branch we're about to discard (i.e. ahead of the current
    // head, after one or more undos), it can never be returned to — mark it unreachable so
    // isAtSavedState() doesn't later report a false "clean" when the new branch happens to reach
    // the same depth. Without this, undo×N then a fresh edit back up to the saved depth would
    // wrongly look saved and the app would skip its save-on-close prompt (silent data loss).
    if (savedDepth_ > static_cast<std::ptrdiff_t>(done_.size())) savedDepth_ = -1;
    // Reserve the slot BEFORE executing. After execute() the document is already mutated,
    // so a reallocation failure at that point would strand the command with nowhere to
    // live. With capacity in hand the push_back below is a noexcept move.
    done_.reserve(done_.size() + 1);
    DocumentChange change{};
    try {
        change = cmd->execute(*doc_);
    } catch (...) {
        // execute() may have mutated partway. The command is the only object that knows
        // how to revert that, so keep it on the undo stack rather than destroying it:
        // dropping it left the document permanently un-undoable past this point. See the
        // exception contract on Command::execute.
        done_.push_back(std::move(cmd));
        undone_.clear();
        // The document really did change and the extent is unknown, so notify the
        // conservative change: an empty region makes the renderer invalidate everything
        // and the panels rebuild.
        doc_->notify(DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer});
        updateDirty();
        throw;
    }
    done_.push_back(std::move(cmd));
    undone_.clear();  // a new edit invalidates the redo branch
    trimToLimit();
    doc_->notify(change);
    updateDirty();
}

void History::undo() {
    if (done_.empty()) return;
    // Same reasoning as push(): reserve the destination before mutating, so the move that
    // follows cannot throw and leave the command homeless.
    undone_.reserve(undone_.size() + 1);
    std::unique_ptr<Command> cmd = std::move(done_.back());
    done_.pop_back();
    DocumentChange change{};
    try {
        change = cmd->undo(*doc_);
    } catch (...) {
        // The undo did not complete, so the command belongs where it was. Putting it on
        // the redo stack would let a later undo skip past a change that is still applied.
        // The pop_back above left capacity, so this cannot throw.
        done_.push_back(std::move(cmd));
        doc_->notify(DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer});
        updateDirty();
        throw;
    }
    undone_.push_back(std::move(cmd));
    doc_->notify(change);
    updateDirty();
}

void History::redo() {
    if (undone_.empty()) return;
    done_.reserve(done_.size() + 1);
    std::unique_ptr<Command> cmd = std::move(undone_.back());
    undone_.pop_back();
    DocumentChange change{};
    try {
        change = cmd->execute(*doc_);
    } catch (...) {
        // A partially re-applied command is in the same position as one whose first
        // execute threw: it belongs on the undo stack, where its undo() can revert
        // whatever it managed to do.
        done_.push_back(std::move(cmd));
        doc_->notify(DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer});
        updateDirty();
        throw;
    }
    done_.push_back(std::move(cmd));
    doc_->notify(change);
    updateDirty();
}

std::string History::topUndoName() const {
    return done_.empty() ? std::string{} : done_.back()->name();
}

std::string History::topRedoName() const {
    return undone_.empty() ? std::string{} : undone_.back()->name();
}

std::vector<std::string> History::undoNames() const {
    std::vector<std::string> names;
    names.reserve(done_.size());
    for (const auto& cmd : done_) names.push_back(cmd->name());  // oldest -> newest
    return names;
}

std::vector<std::string> History::redoNames() const {
    std::vector<std::string> names;
    names.reserve(undone_.size());
    // undone_.back() is the next command redo() replays; list in that replay order.
    for (auto it = undone_.rbegin(); it != undone_.rend(); ++it) names.push_back((*it)->name());
    return names;
}

void History::markSaved() noexcept {
    savedDepth_ = static_cast<std::ptrdiff_t>(done_.size());
    // Saving clears dirty directly (avoid routing through updateDirty's compare).
    doc_->setDirty(false);
}

bool History::isAtSavedState() const noexcept {
    return savedDepth_ == static_cast<std::ptrdiff_t>(done_.size());
}

void History::trimToLimit() {
    if (limit_ == 0) return;  // 0 == unlimited
    while (done_.size() > limit_) {
        done_.erase(done_.begin());
        // The saved point shifts down by one; if it falls off the front, the
        // saved state can never be returned to, so mark it unreachable.
        if (savedDepth_ >= 0) {
            --savedDepth_;
            if (savedDepth_ < 0) savedDepth_ = -1;  // unreachable sentinel
        }
    }
}

void History::updateDirty() {
    doc_->setDirty(!isAtSavedState());
}

}  // namespace pe
