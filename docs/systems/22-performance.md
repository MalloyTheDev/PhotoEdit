# 22 — Performance: Memory, Scratch & Threads

> Milestone: M2+ (continuous) · Status: Spec

## Purpose

This system is the substrate that lets every other system stay fast on large
documents: the RAM budget, the tile cache, the scratch-disk pager, the worker
thread pool, lazy/dirty compositing, and the preview-vs-full-resolution split. It
is the engineering reason a 30,000 × 30,000 px, many-layer document remains usable
on a machine that cannot hold it in memory. Target hardware is 16 GB+ RAM, a
DX12-class GPU, and a fast SSD scratch disk.

## Requirements

**Functional**

- A bounded **RAM budget** for resident [tiles](../adr/0003-tile-based-engine.md);
  cold tiles spill to a **scratch disk** and fault back on demand.
- Reference-counted, copy-on-write tiles shared across [layers](03-layer-system.md),
  [undo](21-history-undo.md), and [snapshots](21-history-undo.md).
- A **worker thread pool** for compositing, filters, I/O, raw decode, thumbnails —
  partitioned by tile so workers never alias.
- **Lazy, dirty-driven** compositing: nothing recomputes unless an input changed.
- **Preview vs full resolution**: interactive proxies, full-res in the background.
- Background job system with progress and cancellation.

**Non-functional**

- `pe_core`, no Qt. Deterministic correctness regardless of cache/scratch state.
- Graceful degradation: low RAM → more scratch paging, still correct, just slower.

## Data model

```cpp
namespace pe {

struct TileRef {                     // shared, immutable-once-published tile
    std::shared_ptr<const TileData> data;   // CoW: copy on write to mutate
};

class TileCache {
public:
    TileRef get(TileKey);            // RAM hit, or fault in from scratch
    void put(TileKey, TileRef);
    void setBudgetBytes(size_t);
    void evictToBudget();            // LRU; spill dirty tiles to scratch first
};

class ScratchDisk {
public:
    void spill(TileKey, const TileData&);   // compressed write
    bool fault(TileKey, TileData& out);     // read back
};

class ThreadPool {
public:
    void parallelForTiles(TileSpan, std::function<void(TileCoord)>);
    JobHandle submit(std::function<void(ProgressSink&, Cancel&)>);
};

} // namespace pe
```

## Behavior & algorithms

**Memory strategy by document size:**

```
small document:  keep most/all tiles resident in RAM
large document:  keep the ACTIVE (visible/edited) tiles resident
                 page INACTIVE tiles to the scratch disk (compressed)
                 recompute previews from resident/proxy data as needed
```

**Tile lifecycle:** allocate lazily (absent tile = transparent); publish as an
immutable `TileRef`; mutation forks a private copy (CoW); the cache evicts by LRU
to honor the budget, spilling dirty tiles to scratch before dropping clean ones
(clean tiles can be recomputed or re-faulted).

**Dirty-driven compositing:** an edit unions a dirty [Rect](../01-master-architecture.md);
`tilesForRect` yields the affected tiles; only those recomposite and re-upload to
the [GPU](23-gpu-acceleration.md). Adjustment/filter changes invalidate the tiles
below them.

**Proxy previews:** for expensive operations (filters, raw develop, transforms),
render a downscaled or viewport-only proxy for interactivity, then schedule the
full-resolution pass on the pool; replace the proxy when ready.

## Interactions

- [Canvas/rendering](02-canvas-rendering.md): consumes dirty tiles, drives repaint.
- [History](21-history-undo.md): shares CoW tiles and the memory budget.
- [Filters](12-filter-engine.md), [transform](10-transform-system.md),
  [raw](16-camera-raw.md): run on the pool, tile-aware with aprons.
- [GPU](23-gpu-acceleration.md): the upload/cache mirror of the tile cache.
- [File I/O](20-file-io.md): load/save/export run as background jobs.

## Performance, threading & GPU

- SIMD pixel kernels (blend, color convert, filters) on the CPU; GPU compute where
  it pays.
- The pool sizes to hardware concurrency; jobs are cancellable and prioritized
  (interactive > background).
- Scratch I/O is asynchronous and compressed to bound disk bandwidth.

## Edge cases & failure modes

- Scratch disk full → surface a clear error, pause spilling, protect the document.
- Budget set below working-set size → thrashing; detect and warn, prefer proxies.
- Concurrent access to a tile being mutated → CoW guarantees readers see a stable
  snapshot.
- Cancellation mid-job must leave the document consistent (no partial commits).

## Testing strategy

- Unit: LRU eviction order; CoW fork-on-write semantics; spill→fault round-trips
  bytes exactly.
- Stress: simulate a working set larger than the budget; verify correctness equals
  the all-RAM result while staying within the budget.
- Concurrency: tile-partitioned parallel composite equals serial composite.

## Phasing

- **M2**: RAM tile cache + budget, CoW tiles, dirty-driven recomposite, worker pool
  for compositing.
- **M5+**: scratch-disk pager; multithreaded/SIMD filters; proxy previews.
- **Continuous**: profiling, budgets tuning, prioritized job scheduling.

## Open questions

- Default budget heuristics (fraction of RAM) and user override UI.
- Scratch compression codec (LZ4 vs zstd) and tile on-disk format.
- Whether the GPU tile cache and CPU tile cache share an eviction policy.

## References (relative links)

- [01 — Master architecture](../01-master-architecture.md) — threading, memory,
  large documents.
- [Glossary](../glossary.md) — Tile, Scratch disk, Dirty region.
- Sibling systems: [02 — Canvas/rendering](02-canvas-rendering.md),
  [23 — GPU](23-gpu-acceleration.md), [21 — History](21-history-undo.md),
  [12 — Filter engine](12-filter-engine.md), [20 — File I/O](20-file-io.md).
- ADRs: [0003 — tile-based engine](../adr/0003-tile-based-engine.md).
