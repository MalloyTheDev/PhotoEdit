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
    // A save in flight captured a point on that same discarded branch, so it is unreachable
    // for the same reason. Dropping it here is what stops commitSave() from marking the NEW
    // branch clean when it happens to reach the depth the old one was saved at.
    if (pendingDepth_ > static_cast<std::ptrdiff_t>(done_.size())) pendingDepth_ = -1;
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
        addBytes(*done_.back());
        for (const auto& c : undone_) {  // the redo branch is discarded here too
            const std::int64_t n = c->retainedBytes();
            bytes_ -= n > 0 ? n : 0;
        }
        if (bytes_ < 0) bytes_ = 0;
        undone_.clear();
        // The document really did change and the extent is unknown, so notify the
        // conservative change: an empty region makes the renderer invalidate everything
        // and the panels rebuild.
        doc_->notify(DocumentChange{DocumentChange::Kind::LayerStructure, Rect{}, kNoLayer});
        updateDirty();
        throw;
    }
    done_.push_back(std::move(cmd));
    addBytes(*done_.back());
    for (const auto& c : undone_) {  // the redo branch is discarded, so stop counting it
        const std::int64_t n = c->retainedBytes();
        bytes_ -= n > 0 ? n : 0;
    }
    if (bytes_ < 0) bytes_ = 0;
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

std::uint64_t History::beginSave() noexcept {
    pendingDepth_ = static_cast<std::ptrdiff_t>(done_.size());
    pendingToken_ = nextSaveToken_++;
    return pendingToken_;
}

void History::abandonSave(std::uint64_t token) noexcept {
    // The save did not produce a file, so nothing is marked. Clearing the point matters
    // anyway: leaving it set would let a later commitSave() with a matching token mark a
    // state no writer ever wrote.
    if (token != 0 && token == pendingToken_) {
        pendingDepth_ = -1;
        pendingToken_ = 0;
    }
}

void History::commitSave(std::uint64_t token) noexcept {
    // A token that does not own the pending point is stale: another save began after this
    // one, or this one was already resolved. Mark nothing, and in particular do NOT clear
    // the point, which belongs to the save still in flight.
    if (token == 0 || token != pendingToken_) {
        updateDirty();
        return;
    }
    const std::ptrdiff_t depth = pendingDepth_;
    pendingDepth_ = -1;
    pendingToken_ = 0;

    // -1 means the written state was undone away or trimmed off the stack while the worker
    // ran, so nothing on this stack is what the file holds. Leave the saved marker where it
    // was: wrong in the safe direction, since the window then offers to save again rather
    // than discarding work it believes is already written.
    if (depth >= 0) savedDepth_ = depth;

    // Through updateDirty, not setDirty(false): the stack may well have moved past this
    // depth while the save was running, and then the document is still dirty.
    updateDirty();
}

bool History::isAtSavedState() const noexcept {
    return savedDepth_ == static_cast<std::ptrdiff_t>(done_.size());
}

void History::addBytes(const Command& c) noexcept {
    const std::int64_t n = c.retainedBytes();
    bytes_ += n > 0 ? n : 0;  // a negative figure would corrupt the running total
}

void History::dropFrontOfDone() noexcept {
    if (done_.empty()) return;
    const std::int64_t n = done_.front()->retainedBytes();
    bytes_ -= n > 0 ? n : 0;
    if (bytes_ < 0) bytes_ = 0;
    done_.erase(done_.begin());
    // The saved point shifts down by one; if it falls off the front, the saved state can
    // never be returned to, so mark it unreachable.
    if (savedDepth_ >= 0) {
        --savedDepth_;
        if (savedDepth_ < 0) savedDepth_ = -1;  // unreachable sentinel
    }
    // The point a save in flight captured shifts with it. Trimming is how a stack already at
    // limit() reindexes under a running save: without this, one stroke made during a save on
    // a full history would leave the pending depth naming a state one command older than the
    // one written, and committing it would mark that stroke clean.
    if (pendingDepth_ >= 0) --pendingDepth_;  // -1 here means it fell off the front
}

void History::dropFurthestRedo() noexcept {
    if (undone_.empty()) return;
    // undone_.back() is the NEXT command redo() replays, so the front is the furthest
    // into the discarded future and the least costly to lose.
    const std::int64_t n = undone_.front()->retainedBytes();
    bytes_ -= n > 0 ? n : 0;
    if (bytes_ < 0) bytes_ = 0;
    undone_.erase(undone_.begin());
}

void History::setByteBudget(std::int64_t bytes) noexcept {
    byteBudget_ = bytes > 0 ? bytes : 0;
    trimToLimit();
}

void History::trimToLimit() {
    if (limit_ != 0) {  // 0 == unlimited
        while (done_.size() > limit_) dropFrontOfDone();
    }
    if (byteBudget_ <= 0) return;  // 0 == unlimited
    // Oldest undo steps go first. Always keep one: a stroke the user just made and cannot
    // undo would be worse than briefly exceeding a soft limit, and a single command can
    // legitimately exceed the whole budget (kMaxStrokeTiles permits about 1 GB in one).
    while (bytes_ > byteBudget_ && done_.size() > 1) dropFrontOfDone();
    // Then the redo branch, which the user has already stepped away from.
    while (bytes_ > byteBudget_ && !undone_.empty()) dropFurthestRedo();
}

void History::updateDirty() {
    doc_->setDirty(!isAtSavedState());
}

}  // namespace pe
