# 01 — Document System

> Milestone: M1 · Status: Spec

## Purpose

The `Document` is PhotoEdit's single source of truth. It is **not "a bitmap"** —
it is the complete editable unit the [glossary](../glossary.md) defines: canvas
geometry and resolution, color mode and bit depth, an ICC color profile, the
layer tree, channels, paths, guides/grids/rulers, artboards, metadata, the
selection, and the history. The visible image is never stored; it is *computed*
by the [compositor](02-canvas-rendering.md) from the layer tree on demand.

Everything else in the engine is defined relative to the Document: a layer is a
node in its tree, a [command](../01-master-architecture.md#2-command--one-reversible-edit)
is a reversible mutation of it, a tool turns input into commands against it, and
the compositor reads it to produce tiles. This document specifies the container,
its invariants, and the discipline — *all mutation through commands, all readers
notified through observers* — that keeps the rest of the 29 systems tractable.

## Requirements

**Functional**

- Hold canvas size (pixels), resolution/pixel density (PPI), `ColorMode`,
  `BitDepth`, and a `ColorProfile` reference.
- Own a `LayerTree` (layers and nested groups), a `ChannelSet`, a `PathSet`, a
  `Selection`, a `Metadata` block, and collections of guides and artboards.
- Track session state: open file path, dirty/unsaved flag, active layer, active
  channel, active tool id, and a save format hint.
- Expose a `History` so callers execute/undo/redo commands; never mutate model
  state except through a command applied via the history.
- Notify registered observers, with a typed change description, after every
  committed mutation (and after non-command session changes like active-layer).
- Support creating a blank document, opening from a decoded file, and reporting
  enough to drive Save / Save As / Export (see [file I/O](20-file-io.md)).
- Permit layers and guides to extend beyond the canvas bounds (the canvas is a
  crop window over an unbounded coordinate space; tile math already handles
  negative coordinates via `floorDiv`).

**Non-functional**

- `pe_core`, pure C++20: **no Qt, no UI types** (see
  [ADR-0006](../adr/0006-headless-core-separation.md)). Qt observers live in the
  [app shell](24-ui-workspace.md).
- A document may be far larger than RAM; the model holds metadata and the layer
  tree, while pixels live in reference-counted, copy-on-write
  [tiles](../adr/0003-tile-based-engine.md) that page to scratch disk. Opening a
  30,000 × 30,000 px document must not allocate its pixels eagerly.
- Mutation happens on one logical owner thread (the "document thread"), so the
  model needs no fine-grained locking (see
  [threading](../01-master-architecture.md#threading-model)).
- Observer notification is cheap and synchronous on the document thread; heavy
  reactions (recomposite, thumbnail) are dispatched to workers by the observer,
  not done inline.

## Data model

Concrete, illustrative shapes in `namespace pe`, building on the `Document`
sketch in [01-master-architecture.md](../01-master-architecture.md#1-document--the-single-source-of-truth).
Real headers may differ in detail, not in concept.

```cpp
namespace pe {

enum class ColorMode : uint8_t {
    RGB = 0,      // the working default
    CMYK = 1,     // print; groundwork M6, full M10
    Gray = 2,
    Lab = 3,
    Indexed = 4,  // palette; export/legacy
    Bitmap = 5,   // 1-bit
};

enum class BitDepth : uint8_t {
    U8  = 8,      // Rgba8 storage path (M1)
    U16 = 16,     // M6
    F32 = 32,     // 32-bit float / HDR, M6
};

// How many channels a mode needs before alpha (drives ChannelSet + storage).
[[nodiscard]] int colorComponentCount(ColorMode) noexcept; // RGB=3, Gray=1, CMYK=4 …

// A guide is an infinite horizontal or vertical line at a document coordinate.
struct Guide {
    enum class Orientation : uint8_t { Horizontal, Vertical };
    Orientation orientation = Orientation::Vertical;
    double position = 0.0;   // x for vertical, y for horizontal, in document px
    bool   locked = false;
};

// Grid / ruler configuration (ruler origin is movable; grid is an overlay).
struct GridSettings {
    double spacing = 64.0;       // major gridline spacing in document px
    int    subdivisions = 4;     // minor lines per major cell
    Point  rulerOrigin{0, 0};    // where 0,0 sits on the rulers
    bool   showGrid = false;
    bool   showRulers = true;
    bool   showGuides = true;
    bool   snapToGuides = true;
};

// A named sub-canvas with its own bounds (multi-screen / asset design).
struct Artboard {
    std::string name;
    Rect        bounds;          // in document coordinates
    Rgba8       backgroundColor; // optional fill behind the artboard's layers
    bool        clipToBounds = true;
};

// Non-pixel descriptive data. EXIF/XMP/IPTC are preserved verbatim where we
// don't model a field, so round-trips don't silently drop camera/copyright data.
struct Metadata {
    std::string title;
    std::string author;
    std::string copyright;
    std::string description;
    std::vector<std::string> keywords;
    int64_t createdUnixSec = 0;
    int64_t modifiedUnixSec = 0;
    std::map<std::string, std::string> xmp;   // raw XMP packets / extra fields
    std::vector<std::byte> exifBlob;          // opaque, preserved on save
};

// Typed description of what changed, delivered to observers after a commit.
struct DocumentChange {
    enum class Kind : uint8_t {
        Pixels,        // a region of one or more layers changed
        LayerStructure,// add/remove/reorder/group
        LayerProps,    // opacity/blend/visibility/name/lock/mask
        Selection,
        Channels, Paths, Guides, Artboards, Metadata,
        ColorMode, BitDepth, Profile, CanvasSize,
        ActiveLayer, ActiveChannel, ActiveTool, DirtyState,
    };
    Kind kind = Kind::Pixels;
    Rect dirtyRegion;          // valid for Kind::Pixels; the union of touched tiles
    LayerId layer = 0;         // affected layer where meaningful
};

class DocumentObserver {
public:
    virtual ~DocumentObserver() = default;
    virtual void onDocumentChanged(const Document&, const DocumentChange&) = 0;
};

class Document {
public:
    // --- geometry & color ---
    [[nodiscard]] Size canvasSize() const noexcept;
    [[nodiscard]] int  resolutionPpi() const noexcept;
    [[nodiscard]] ColorMode colorMode() const noexcept;
    [[nodiscard]] BitDepth  bitDepth() const noexcept;
    [[nodiscard]] const ColorProfile& profile() const noexcept;

    // --- the model (read-only views; mutate via commands) ---
    [[nodiscard]] const LayerTree&  layers() const noexcept;
    [[nodiscard]] const ChannelSet& channels() const noexcept;
    [[nodiscard]] const PathSet&    paths() const noexcept;
    [[nodiscard]] const Selection&  selection() const noexcept;
    [[nodiscard]] const Metadata&   metadata() const noexcept;
    [[nodiscard]] std::span<const Guide>    guides() const noexcept;
    [[nodiscard]] std::span<const Artboard> artboards() const noexcept;
    [[nodiscard]] const GridSettings& grid() const noexcept;

    // --- session state ---
    [[nodiscard]] LayerId activeLayer() const noexcept;
    [[nodiscard]] std::optional<std::filesystem::path> filePath() const;
    [[nodiscard]] bool isDirty() const noexcept;

    // --- mutation entry point ---
    History& history() noexcept;                 // execute/undo run through here
    void addObserver(DocumentObserver*);
    void removeObserver(DocumentObserver*);

private:
    friend class History;                        // History applies commands + notifies
    void notify(const DocumentChange&);          // called after each committed change
    void setDirty(bool);

    Size canvasSize_{};
    int  resolutionPpi_ = 72;
    ColorMode colorMode_ = ColorMode::RGB;
    BitDepth  bitDepth_  = BitDepth::U8;
    ColorProfileRef profile_;
    LayerTree  layers_;
    ChannelSet channels_;
    PathSet    paths_;
    Selection  selection_;
    Metadata   metadata_;
    std::vector<Guide>    guides_;
    std::vector<Artboard> artboards_;
    GridSettings grid_;
    LayerId  activeLayer_ = 0;
    History  history_;
    bool     dirty_ = false;
    std::optional<std::filesystem::path> filePath_;
    std::vector<DocumentObserver*> observers_;
};

} // namespace pe
```

## Behavior & algorithms

**Mutation always flows through a command.** Public read accessors return `const`
views; there is no public setter. To change anything, a caller (tool, menu
action, script) constructs a `Command` and submits it to `history()`. The
`History` calls `command.execute(doc)`, which uses a private/`friend` mutation
surface, then the document emits a `DocumentChange` to observers and sets the
dirty flag. Undo is symmetric: `command.undo(doc)` restores prior state and emits
the inverse change. This is the
[ADR-0005](../adr/0005-command-history-model.md) backbone — undo, history,
[actions](26-automation.md), and scripting are one mechanism.

```
applyCommand(doc, cmd):                # inside History
    cmd.execute(doc)                   # mutates via friend surface, records undo data
    doc.notify(cmd.changeDescription())# typed DocumentChange (e.g. Pixels + region)
    doc.setDirty(true)
    historyStack.push(cmd)
```

**Creating a document.** `Document::createBlank(size, mode, depth, ppi, profile)`
builds an empty `LayerTree` with a single background or transparent base layer,
seeds the default `ChannelSet` for the mode (R/G/B/A for RGB; see
[channels](19-channels.md)), and sets `dirty=false` with no file path. No pixel
tiles are allocated until something writes to them — an all-transparent layer is
simply an absence of tiles.

**Opening a file.** [File I/O](20-file-io.md) decodes a file into a fully formed
`Document` off the document thread, then hands ownership over. The native format
restores the entire model losslessly; PSD/PSB restores the common structure;
flat raster formats yield a single-layer document with the embedded profile (or
an assumed one). The document records its `filePath_` and `dirty=false`.

**Mode / depth / profile changes are commands too**, because they are reversible
and must round-trip through history. A `ConvertColorModeCommand` runs the managed
conversion (via [color management](15-color-management.md)) on every layer's
tiles and rewrites the `ChannelSet`; a `ConvertBitDepthCommand` re-quantizes
storage. Both can be expensive and are dirty-region-trivial (they touch
everything), so they snapshot rather than tile-delta where cheaper.

**Active-state changes** (active layer, active channel, active tool, guide drag
preview) are lightweight. Most are *not* undoable session state and bypass the
history, but they still emit a `DocumentChange` so panels stay in sync. The
boundary is deliberate: changing *what you're editing* is not an edit; changing
*the pixels/structure* is.

**Resolution vs. canvas size.** Resolution (PPI) is metadata describing physical
print density; it does not change pixel counts. Canvas resizing (crop, canvas
size) *does* change pixel bounds and is a command that may add/remove tiles and
adjust layer/guide coordinates.

## Interactions

- **[Layer system](03-layer-system.md):** the `LayerTree` is the document's
  centerpiece; the document owns it and exposes a read-only view. All layer
  mutations are commands.
- **[Compositor / canvas](02-canvas-rendering.md):** the compositor reads the
  document (tree + profile + bit depth) to produce tiles; it never mutates it. A
  `DocumentChange{Pixels, region}` is exactly what tells the canvas which tiles
  to recomposite.
- **[Command / History](../01-master-architecture.md#2-command--one-reversible-edit):**
  the only mutation path; see [ADR-0005](../adr/0005-command-history-model.md)
  and [history](21-history-undo.md).
- **[Channels](19-channels.md)** and **[masks](06-masks.md):** the `ChannelSet`
  and per-layer masks are owned through the tree; saved selections become alpha
  channels.
- **[Selection](07-selection-system.md):** the active selection lives on the
  document and gates painting/fill/filter operations.
- **[Color management](15-color-management.md):** owns the `ColorProfile` and the
  working space the document composites in.
- **[File I/O](20-file-io.md)** + **[ADR-0007](../adr/0007-native-document-format.md):**
  constructs/serializes the whole model; the document supplies the dirty flag and
  format hint that drive save/export UI.
- **App shell:** Qt panels (Layers, Navigator, Histogram, Info) register as
  `DocumentObserver`s at the boundary and translate `DocumentChange` into widget
  refreshes; the document knows nothing of Qt.

## Performance, threading & GPU

- The document object itself is small: tree nodes, metadata, and references. The
  bulk (pixels) lives in tiles managed by the [performance layer](22-performance.md),
  reference-counted and copy-on-write, paged to scratch under a RAM budget.
- Single-owner mutation means **no locks** on the model. Workers read *immutable
  snapshots* of tiles handed to them by tile addresses, never the live tree.
- Observer notification is synchronous and must stay O(small): it ships a
  `DocumentChange`, not pixels. Recompositing and GPU upload happen downstream on
  the render/worker threads, keyed off the change's `dirtyRegion`.
- The document holds **no GPU resources**. GPU textures for display tiles are a
  cache the [RHI](23-gpu-acceleration.md) maintains, invalidated per dirty tile.

## Edge cases & failure modes

- **Empty document / no layers:** the compositor yields transparent; the canvas
  shows the checkerboard. Deleting the last layer is allowed (or guarded by UI),
  but the model must tolerate an empty tree.
- **Layers beyond the canvas:** legal; coordinates can be negative. Crop/canvas
  resize must not corrupt off-canvas pixels (they are retained unless flattened).
- **Open failure / partial decode:** no `Document` is produced; the previous
  document is untouched. PSD features we don't model are skipped with a warning,
  never a crash (faithful-enough interchange per
  [ADR-0007](../adr/0007-native-document-format.md)).
- **Mode/depth conversion is lossy** (e.g. RGB→Indexed, 16→8 bit): the command is
  still reversible *as a history step* by snapshotting pre-conversion tiles, but
  re-converting forward will not recover discarded precision — UI warns.
- **Unsaved-changes lifecycle:** any committed command sets `dirty=true`; a
  successful save clears it; undoing back to the last-saved point should clear it
  (history tracks the saved marker).
- **Resolution = 0 / degenerate canvas:** rejected at construction; `Size::isEmpty`
  and a positive PPI are invariants.

## Testing strategy

Headless `pe_core` unit tests (the dependency-free harness):

- **Construction invariants:** blank document has the right default channels for
  each `ColorMode`, positive PPI, `dirty=false`, one base layer, zero tiles
  allocated.
- **Mutation discipline:** applying a command sets dirty, pushes history, and
  emits exactly one `DocumentChange` of the expected `Kind` with the correct
  `dirtyRegion` (assert the region equals the union of touched tiles via
  `tilesForRect`).
- **Observer contract:** a recording observer sees notifications in order;
  add/remove observer is safe mid-notification; undo emits the inverse change.
- **Dirty/saved marker:** edit → dirty; "mark saved" → clean; undo across the
  saved marker toggles dirty correctly.
- **Off-canvas coordinates:** guides and layer tiles at negative coordinates
  survive a canvas resize round-trip.
- **Conversion commands:** mode/depth conversion then undo restores byte-exact
  tiles (snapshot path), validated on a small fixture.

Golden-image coverage is indirect here: it lives in the compositor/layer/blend
specs, which consume the document the construction tests build.

## Phasing

- **M1 (first version):** RGB / 8-bit (`Rgba8`) document; layer tree, selection,
  metadata, guides; commands for create/add/delete/duplicate/reorder layer and
  layer props; dirty flag; observers. Channels are the implicit R/G/B/A set.
  Artboards, CMYK/Lab, and 16/32-bit are *modeled in the data shapes but inert*.
- **M6:** real 16-bit and 32-bit-float paths; profile-aware conversions; the
  [channels](19-channels.md) panel surfaces; Lab and CMYK groundwork.
- **M7:** full native-format round-trip and PSD/PSB import populate every field
  above; metadata (EXIF/XMP) preserved.
- **M8+:** artboards become first-class (per-artboard export), smart-object and
  text/vector layers extend the tree, Indexed/Bitmap export modes.

## Open questions

- **Active-state undo granularity:** should active-layer changes be coalesced
  *into* the next pixel command for nicer undo stepping, or always excluded?
  Leaning excluded, but interactive feel may argue otherwise.
- **Multiple documents & shared assets:** a future asset library (linked smart
  objects, shared swatches) implies cross-document references — out of scope for
  M1 but the `Document` should not assume it is the universe.
- **Per-artboard color profiles:** likely unnecessary; revisit when print/export
  per artboard lands in M10.
- **History persistence:** the native format *may* store history; whether a
  reopened document restores its undo stack is deferred to
  [file I/O](20-file-io.md) / [history](21-history-undo.md).

## References

- [01 — Master Architecture](../01-master-architecture.md) — the `Document`,
  `Command`, and `Layer` contracts and the data-flow this elaborates.
- [00 — Vision & Scope](../00-vision-and-scope.md) — "a real document model, not a
  bitmap"; non-destructive principle.
- [Glossary](../glossary.md) — Document, Canvas, Layer, Channel, Selection,
  Artboard, Non-destructive.
- ADRs: [0003 — tile-based engine](../adr/0003-tile-based-engine.md),
  [0005 — command/history model](../adr/0005-command-history-model.md),
  [0006 — headless core](../adr/0006-headless-core-separation.md),
  [0007 — native document format](../adr/0007-native-document-format.md).
- Sibling systems: [layers](03-layer-system.md),
  [canvas & rendering](02-canvas-rendering.md), [channels](19-channels.md),
  [masks](06-masks.md), [selection](07-selection-system.md),
  [color management](15-color-management.md), [file I/O](20-file-io.md),
  [history](21-history-undo.md).
