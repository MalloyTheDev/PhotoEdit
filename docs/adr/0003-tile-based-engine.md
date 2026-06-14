# ADR-0003 — Tile-based pixel storage & dirty-region rendering

**Status:** Accepted

## Context

A flagship editor must stay responsive on documents far larger than a screen — or
than RAM. A naive "one contiguous buffer per layer, recompute the whole image on
every edit" design collapses on a 30,000 × 30,000 px, many-layer document: every
brush dab would touch hundreds of megabytes and every repaint would recomposite
the entire canvas.

## Decision

Store layer pixels as fixed-size **256 × 256 tiles**, and make the tile the unit
of every large-buffer operation:

- **Dirty tracking** — edits mark only the tiles they touch dirty.
- **Compositing** — the compositor recomputes only dirty tiles.
- **GPU upload** — only changed tiles are re-uploaded.
- **Undo** — history stores per-tile deltas, not whole-layer copies.
- **Memory** — tiles are reference-counted and copy-on-write; a RAM budget bounds
  resident tiles and cold tiles spill to the scratch disk.
- **Threading** — work is partitioned by tile so workers never alias pixels.

`kTileSize = 256` is chosen to amortize per-tile overhead while keeping a single
brush dab's footprint small and GPU uploads cheap. It is a single constant
(`pe/core/Tile.hpp`) so it can be tuned with data.

## Consequences

**Positive**
- Edit cost scales with the *changed* area, not the document size.
- Large-document support and undo memory efficiency are properties of the
  foundation, not per-feature work.
- Natural parallelism and a natural GPU upload granularity.
- Copy-on-write tiles make snapshots/undo cheap (unchanged tiles are shared).

**Negative / costs**
- More complex than a flat buffer: tile boundaries must be handled in brushes,
  filters (halo/overlap at edges), transforms, and selection math.
- Filters with wide kernels need neighboring-tile context (apron/halo handling).
- A tile cache, eviction policy, and scratch-disk pager must exist early.

## Alternatives considered

- **Flat per-layer buffers.** Simpler to write and fine for small images;
  rejected because it cannot meet the large-document and undo-memory goals.
- **Quadtree / sparse adaptive tiles.** More memory-optimal for mostly-empty
  layers, but more complex; fixed-grid tiles with sparse allocation (absent tiles
  = transparent) capture most of the benefit at lower complexity. We allocate
  tiles lazily, approximating sparsity.

## Notes

Tile math primitives (`tileBounds`, `tilesForRect`, `floorDiv`) and the constant
ship in M0 and are unit-tested. The cache/scratch/eviction machinery lands in M2+
(see [systems/22-performance.md](../systems/22-performance.md)).
