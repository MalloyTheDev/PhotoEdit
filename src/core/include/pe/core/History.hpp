#pragma once

#include "pe/core/Command.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pe {

class Document;

// Default cap on what the undo and redo stacks may retain, together.
//
// The step count alone never bound anything useful. A 300 px brush over a 4000 px drag
// touches on the order of 57 tiles, about 15 MB a stroke, so the hundred-step default
// allowed roughly 1.5 GB; and kMaxStrokeTiles permits about 1 GB in a SINGLE step, so a
// hundred of those is far past any machine. The failure mode was memory exhaustion from
// ordinary painting.
//
// 1 GiB is chosen to be generous enough that a normal session never trims, while keeping
// history well clear of the loader's own 3.7 GiB content budget, since the two add up.
inline constexpr std::int64_t kDefaultHistoryBytes = 1LL << 30;

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

    // Full entry lists for a History panel timeline. undoNames() is oldest-first
    // (the order the commands were applied; size == undoDepth()). redoNames() is
    // next-to-redo first (the order redo() will replay them; size == redoDepth()).
    [[nodiscard]] std::vector<std::string> undoNames() const;
    [[nodiscard]] std::vector<std::string> redoNames() const;

    // Bound the number of retained undo steps (memory). Default 100. A limit of
    // 0 means UNLIMITED (no trimming) — be deliberate, as unbounded history also
    // retains each command's payload (e.g. removed layers) and can grow without
    // bound; never drive this from untrusted input.
    void setLimit(std::size_t n) noexcept { limit_ = n; }
    [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

    // Bound the BYTES retained by the undo and redo stacks together, which is the limit
    // that actually binds: a step count cannot bound memory when one step can be a
    // gigabyte. kDefaultHistoryBytes by default; 0 means unlimited, which is a deliberate
    // choice and never something to drive from untrusted input.
    //
    // Trimming drops the oldest undo steps first, then the furthest-away redo steps. One
    // step is always kept even if it alone exceeds the budget: a stroke the user just made
    // and cannot undo would be worse than briefly exceeding a soft limit.
    void setByteBudget(std::int64_t bytes) noexcept;
    [[nodiscard]] std::int64_t byteBudget() const noexcept { return byteBudget_; }

    // What the two stacks currently retain, by the same approximation the budget uses.
    [[nodiscard]] std::int64_t retainedBytes() const noexcept { return bytes_; }

    // Saved-state tracking: call after a successful save.
    void markSaved() noexcept;

    // Saving a snapshot, in two halves.
    //
    // A save serializes a snapshot taken at the moment the user asked for it, and the user
    // keeps painting while the worker writes. Calling markSaved() when the worker finishes
    // would claim the strokes made DURING the save are on disk, and the window would stop
    // offering to save them: silent data loss, in the one place the whole feature exists to
    // protect.
    //
    // The written state cannot be a depth the CALLER holds either. undo(), a new branch and
    // trimToLimit() all reindex this stack while the worker runs, so a plain integer taken
    // before the save silently comes to mean a different state: undo twice and repaint back
    // to the same depth, or push one command onto a stack already at limit(), and committing
    // that number marks unsaved work clean. beginSave() therefore records the point INSIDE
    // the history, where every one of those operations already knows to shift or invalidate
    // it, and hands back an opaque token.
    //
    // Pass that token to commitSave() once the bytes are on disk. It commits only if the
    // point it named is still reachable and still the newest save in flight; otherwise the
    // document stays dirty, which is the safe direction to be wrong in. A save that failed
    // or threw calls abandonSave() instead. Both end the in-flight save, so exactly one of
    // them must follow every beginSave().
    [[nodiscard]] std::uint64_t beginSave() noexcept;
    void commitSave(std::uint64_t token) noexcept;
    void abandonSave(std::uint64_t token) noexcept;

    [[nodiscard]] bool isAtSavedState() const noexcept;

private:
    void trimToLimit();
    void updateDirty();

    // Sum of retainedBytes() over both stacks, maintained incrementally.
    void addBytes(const Command& c) noexcept;
    void dropFrontOfDone() noexcept;
    void dropFurthestRedo() noexcept;

    Document* doc_;
    std::vector<std::unique_ptr<Command>> done_;
    std::vector<std::unique_ptr<Command>> undone_;
    std::size_t limit_ = 100;
    std::int64_t byteBudget_ = kDefaultHistoryBytes;
    std::int64_t bytes_ = 0;
    // done_.size() at the last save, or -1 if the saved state was trimmed away
    // (and thus can never be returned to → always dirty).
    std::ptrdiff_t savedDepth_ = 0;
    // The save currently in flight: the depth it captured, tracked by the same rules as
    // savedDepth_, and the token that names it. Depth -1 / token 0 means none, which is
    // also what an invalidated point becomes.
    std::ptrdiff_t pendingDepth_ = -1;
    std::uint64_t pendingToken_ = 0;
    std::uint64_t nextSaveToken_ = 1;  // 0 is reserved for "no save in flight"
};

}  // namespace pe
