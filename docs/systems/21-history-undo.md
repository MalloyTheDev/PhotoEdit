# 21 — History, Undo & Snapshots

> Milestone: M3 · Status: Spec

## Purpose

Undo in a professional editor is far more than Ctrl+Z: it tracks brush strokes,
layer create/delete/reorder, mask edits, filter applications, transforms,
selection changes, text edits, document resizes, and color adjustments — across
documents that are too large to copy per step. PhotoEdit implements this with the
**command model** from [ADR-0005](../adr/0005-command-history-model.md): every
mutation is a reversible `Command`, so interactive undo, the History panel,
snapshots, [actions, and batch/scripting](26-automation.md) are all one mechanism.

## Requirements

**Functional**

- Linear undo/redo over all mutations; a configurable history depth.
- A **History panel**: a list of states; click to jump; non-linear history option.
- **Snapshots**: named, retained restore points independent of the rolling depth.
- Commands carry a human-readable name for the panel ("Brush Tool", "Gaussian
  Blur").
- Coalescing: a single drag (one brush stroke, one transform) is one undo step.

**Non-functional**

- `pe_core`, no Qt; the History panel UI lives in the [app shell](24-ui-workspace.md).
- **Memory-bounded**: pixel edits store tile deltas (copy-on-write), not whole
  layers, so undo memory scales with edited area, not document size.
- Mutation and history live on the document thread (no locking).

## Data model

```cpp
namespace pe {

class Command {
public:
    virtual ~Command() = default;
    virtual std::string name() const = 0;
    virtual void execute(Document&) = 0;
    virtual void undo(Document&) = 0;
    virtual bool serialize(Writer&) const = 0;  // for actions/scripting
    virtual size_t approximateCost() const { return 0; }
    // Optional: merge an immediately-following compatible command (e.g. same
    // drag) to coalesce into one undo step.
    virtual bool mergeWith(const Command&) { return false; }
};

// The before-image of tiles a pixel command overwrote (CoW: shares unchanged).
struct TileDelta { LayerId layer; std::vector<std::pair<TileCoord, TileRef>> before; };

struct Snapshot { std::string name; DocumentState state; };  // lightweight refs

class History {
public:
    void push(std::unique_ptr<Command>);   // executes + truncates redo
    bool canUndo() const; bool canRedo() const;
    void undo(Document&); void redo(Document&);
    void jumpTo(size_t index, Document&);
    Snapshot makeSnapshot(std::string name);
    void restore(const Snapshot&, Document&);
private:
    std::vector<std::unique_ptr<Command>> done_;
    std::vector<std::unique_ptr<Command>> undone_;
    size_t limit_ = 100;
};

} // namespace pe
```

## Behavior & algorithms

**Implementation strategy — hybrid.** We combine approaches per mutation kind:

- **Tile-delta** for pixel mutations (brush, fill, filter bake, retouch): before
  `execute`, snapshot the [tiles](../adr/0003-tile-based-engine.md) about to
  change via copy-on-write (unchanged tiles are shared, so the cost is the edited
  area only). `undo` restores those tile refs.
- **State commands** for structural changes (add/delete/reorder layer, transform,
  edit adjustment params, change selection): store the small before/after state.
  Undo of "delete layer" restores the layer object; undo of a transform restores
  the prior matrix; undo of an adjustment restores prior parameters.
- **Snapshots** capture a full lightweight document state (layer tree + tile refs,
  again sharing via CoW) as a named restore point.

```
push(cmd):
    cmd.execute(doc)
    if !done_.empty() and done_.back().mergeWith(cmd): return   # coalesce drag
    undone_.clear()                                             # new branch
    done_.push_back(cmd)
    if done_.size() > limit_: evictOldest()                     # see memory mgmt
```

**Memory pressure.** When history memory exceeds budget, the oldest steps are
dropped (their tile deltas freed); recomputable deltas may be discarded in favor
of re-execution. Snapshots are retained until explicitly deleted. This ties into
the [performance](22-performance.md) budget and scratch disk.

**Non-linear history** (optional) keeps the tree of states so jumping back and
editing creates a branch rather than truncating.

## Interactions

- Every mutating system ([brush](08-brush-engine.md), [filters](12-filter-engine.md),
  [transform](10-transform-system.md), [layers](03-layer-system.md),
  [selection](07-selection-system.md), [text](17-text-typography.md)) pushes
  commands here — it is the universal sink for edits.
- [Automation](26-automation.md): actions are recorded serialized commands; batch
  replays them headlessly.
- [Performance](22-performance.md): shares the CoW tile machinery and budget.
- [UI/workspace](24-ui-workspace.md): the History panel observes this stack.

## Performance, threading & GPU

- CoW tile capture is O(changed tiles); no full-layer copies.
- Heavy command *execution* (a filter) may dispatch to workers, but the history
  bookkeeping stays on the document thread.

## Edge cases & failure modes

- Coalescing must not merge across tool/selection changes → strict `mergeWith`.
- Redo after a new edit is discarded (linear mode) → clear UI affordance.
- Document resize/crop changes tile geometry → commands store enough to reverse.
- Out-of-memory history → evict oldest, never fail the edit itself.
- Non-reversible external effects (e.g. a cloud call) are modeled as state, not
  re-invoked on redo.

## Testing strategy

- Unit: execute→undo restores exact bytes for each command type (tile-delta and
  state); redo re-applies; merge coalesces a simulated drag into one step.
- Property: random sequences of commands then full undo returns the document to
  its initial state (round-trip).
- Memory: undo of a small brush stroke on a huge document allocates ~stroke-sized
  deltas, not document-sized.

## Phasing

- **M3**: command stack, undo/redo, tile-delta for paint, coalescing, History
  panel, basic snapshots.
- **M5+**: state commands for adjustments/filters; memory-pressure eviction.
- **M10**: non-linear history; serialization feeding [actions](26-automation.md).

## Open questions

- Default history depth and per-document memory budget.
- Non-linear history UI/storage cost vs benefit.
- Persisting history into the native document (optional) and its size cost.

## References (relative links)

- [01 — Master architecture](../01-master-architecture.md) — the `Command` contract.
- [Glossary](../glossary.md) — Command, History, Tile, Scratch disk.
- Sibling systems: [22 — Performance](22-performance.md),
  [26 — Automation](26-automation.md), [08 — Brush engine](08-brush-engine.md),
  [12 — Filter engine](12-filter-engine.md), [24 — UI/workspace](24-ui-workspace.md).
- ADRs: [0005 — command/history model](../adr/0005-command-history-model.md),
  [0003 — tile-based engine](../adr/0003-tile-based-engine.md).
