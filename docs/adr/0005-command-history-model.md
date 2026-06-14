# ADR-0005 — Command-based mutation & tile-delta history

**Status:** Accepted

## Context

Undo/redo is table stakes, but in a professional editor the same need recurs in
four places: interactive undo, the History panel/snapshots, recorded **actions**,
and **batch/scripting**. If these are built separately they drift and multiply
bugs. We also can't store full-document copies per undo step on large documents.

## Decision

Make **every mutation a `Command`** with `execute`, `undo`, `serialize`, and a
cost hint. All four needs are then one mechanism:

- **Undo/redo** = walking the command stack.
- **History/snapshots** = labeled points in that stack.
- **Actions** = a recorded, replayable list of serialized commands.
- **Batch/scripting** = constructing and running commands programmatically.

Memory is bounded with **tile-delta undo**: pixel-mutating commands record the
*prior contents of the tiles they touch* (copy-on-write, so unchanged tiles are
shared), not whole layers. Structural commands (add/delete/reorder/transform/edit
parameters) store the small before/after state they need.

## Consequences

**Positive**
- One well-tested machine powers undo, history, automation, and scripting.
- Undo memory scales with edited area, not document size (via tile deltas + CoW).
- Serializable commands make actions and macro recording almost free.

**Negative / costs**
- Every feature author must express mutations as commands (discipline, some
  boilerplate). Worth it; it is the backbone of the architecture.
- Long histories on huge edits still cost memory; bounded by history limits,
  snapshot coalescing, and dropping recomputable deltas under pressure.

## Alternatives considered

- **Full-state snapshots per step.** Trivial to implement, impossible on large
  documents. Rejected (kept only as occasional snapshots).
- **Ad-hoc per-feature undo.** Fast to start, unmaintainable at scale; would not
  yield actions/scripting for free. Rejected.

## Notes

See [systems/21-history-undo.md](../systems/21-history-undo.md) and the
`Command` contract in [01-master-architecture.md](../01-master-architecture.md).
Automation builds directly on this ([systems/26-automation.md](../systems/26-automation.md)).
