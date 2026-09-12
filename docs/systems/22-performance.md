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

### Threading model as implemented

One thread owns a `Document`: the thread that created it and mutates it, which in the
application is the GUI thread. That ownership is the whole model, and it covers READS as
well as writes. `TileStoreT` caches its content bounds lazily behind `mutable`,
`CanvasRenderer` owns a mutable LRU and a single shared per-tile scratch buffer, and
`Document::notify` dispatches observer callbacks synchronously on the calling thread. Two
threads reading one document race on all three.

The one supported way for a second thread to see document state is
`Document::snapshot()`. It returns a `unique_ptr<const Document>` holding its own layer
tree whose tile buffers are shared copy-on-write with the live document, and whose
identities match it. Nothing else references that object, so a worker may read it freely
while the owning thread keeps editing.

What makes that safe is the write barrier, not a reference count. `TileStoreT` marks a
tile shared when its buffer escapes (handed out by `sharedTile`, copied by a snapshot or
a layer clone, installed by `setTile`), and forks a marked tile before mutating it in
place. `use_count()` is not used to decide, deliberately: it answers "how many owners
exist at this instant", which another thread can invalidate between the test and the
write. The shared flag is set by the owning thread before the worker exists and is
monotone within a buffer's life, so a stale flag costs one unnecessary fork and can never
cost correctness.

### The snapshot contract

What `Document::snapshot()` covers is a contract, not an implementation detail, because a
consumer that assumes more gets silence rather than a failure:

| state | in a snapshot | why |
|---|---|---|
| canvas size, color mode, bit depth, resolution, profile | copied | serialized |
| layer tree, properties, blend/opacity/locks/clipped | copied | serialized |
| layer ids | preserved | an id a caller holds must resolve, starting with the active-layer marker |
| pixel tiles | shared copy-on-write | the whole point; pointer copies, not pixels |
| layer masks | deep-copied | `MaskBuffer` stores its tiles by value |
| history | excluded | not serialized |
| selection | excluded | not serialized, and `Selection` stores mask tiles by value (up to ~268 MB) |
| observers, dirty flag | excluded | session state |
| `CanvasRenderer` | excluded | single-threaded per instance; see the note above |

The split is safe only while the excluded half is state persistence never reads.
`tests/core/test_snapshot.cpp` enforces exactly that: it serializes a document exercising
every feature the `.pedoc` writer emits, serializes its snapshot, and requires the two byte
streams to be identical. Add a field to `Document` or `Layer`, persist it, and forget
`snapshot()`, and that test goes red. It is a whole-file comparison rather than a property
list on purpose, because a list has to be kept in step by hand, which is the failure it
exists to prevent.

Rules that follow, and that a new background operation has to satisfy:

- Take the snapshot on the owning thread. Never from a worker.
- A worker reads only its snapshot. A worker that needs the renderer, the selection or
  history is not snapshot-capable today: those are not captured, and `CanvasRenderer` in
  particular is single-threaded per-instance.
- The GUI side of this lives in `pe::app::runDocumentTask`, whose `TaskAccess` states
  which of the three shapes an operation is: `Snapshot` (worker owns a copy, the user
  keeps painting), `LiveDocument` (worker touches the real document, so input is blocked
  and the canvas frozen) or `Detached` (a new document is being built).

### Data-parallel kernels (as implemented)

Filters and adjustments parallelize WITHIN a single operation, a different axis from the
snapshot model above: no second thread touches the `Document`. The owning thread splits one
CPU-bound pass across a few lanes and joins before returning. The primitive is
`pe::parallelFor(begin, end, minChunk, body)` (`src/core/src/Parallel.cpp`): it divides
`[begin, end)` into contiguous, disjoint sub-ranges, runs `body(lo, hi)` on each (worker
threads plus the calling thread), and joins. `parallelThreadCount()` sizes the split to
`hardware_concurrency()` (capped at 64); the `PHOTOEDIT_THREADS` environment variable
overrides it, which the tests use to force a specific lane count.

The determinism contract is structural, not incidental. `body` writes only outputs indexed
by its own `[lo, hi)` and shares nothing mutable, so each output element is produced by
exactly one invocation using the same arithmetic the serial loop would use. There is no
reduction and no ordering to preserve, so the result is BYTE-IDENTICAL for any lane count,
including one. `tests/core/test_parallel.cpp` pins that: every parallelized kernel is
compared bit-for-bit at 1, 2, 4 and 8 lanes, and the baked tile-delta path is compared
end-to-end.

What runs on it today (`src/core/src/Filter.cpp`):

- the separable convolution (box and Gaussian blur, and Unsharp Mask built on it), Find
  Edges, the median filter, Mosaic and Add Noise, each split into row bands (Mosaic into
  cell-row bands, so a whole cell stays on one lane). Add Noise was already a pure hash of
  pixel position, so it parallelizes without changing a byte;
- the bake tile-delta diff in `bakePixelEditImpl`, which every destructive filter and
  adjustment funnels through: one job per output tile, each writing its own result slot,
  merged back in tile order. `store.sharedTile()` flips a per-tile `mutable` flag, but each
  tile coordinate is visited by exactly one job so those writes never collide, and
  `std::map::find` is a concurrent-safe const read.

The COMPOSITOR is deliberately NOT on this path. `CanvasRenderer` holds a single mutable
scratch buffer and an unsynchronized LRU, `TileStoreT::contentBounds()` caches lazily behind
`mutable`, and `Document::notify` runs observers synchronously; parallelizing a composite
would race all three. Measured on a 24 MP buffer (16 cores), the compute-bound kernels scale
well (Find Edges about 7.5x, median about 5.9x, Add Noise about 10.6x); the separable blur is
memory-bandwidth-bound across its four full-image passes and gains less (about 1.8x).

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
- **M5+**: scratch-disk pager; multithreaded filters landed early via data-parallel
  kernels (#170, see above); SIMD filters and proxy previews still pending.
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
