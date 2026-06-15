#include "pe/core/History.hpp"

#include "pe/core/Document.hpp"

namespace pe {

void History::push(std::unique_ptr<Command> cmd) {
    if (!cmd) return;
    const DocumentChange change = cmd->execute(*doc_);
    done_.push_back(std::move(cmd));
    undone_.clear();  // a new edit invalidates the redo branch
    trimToLimit();
    doc_->notify(change);
    updateDirty();
}

void History::undo() {
    if (done_.empty()) return;
    std::unique_ptr<Command> cmd = std::move(done_.back());
    done_.pop_back();
    const DocumentChange change = cmd->undo(*doc_);
    undone_.push_back(std::move(cmd));
    doc_->notify(change);
    updateDirty();
}

void History::redo() {
    if (undone_.empty()) return;
    std::unique_ptr<Command> cmd = std::move(undone_.back());
    undone_.pop_back();
    const DocumentChange change = cmd->execute(*doc_);
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
