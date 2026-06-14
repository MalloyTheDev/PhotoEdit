# 19 — Channels

> Milestone: M6 · Status: Spec

## Purpose

A **channel** is a single component plane of the document's pixel data. Together they
are how an image's color and coverage are actually stored: the **color channels**
(R/G/B, or C/M/Y/K, or Gray, or L/a/b) that depend on the document's
[color mode](01-document-system.md), the **alpha channel** for transparency, **spot
channels** (named inks for print), and **saved-selection** channels (a stashed
[selection](07-selection-system.md) kept as grayscale).

Channels matter because they are the seam where several systems meet the raw data.
A [selection](07-selection-system.md) is *saved as* an alpha channel; a
[mask](06-masks.md) is channel-like grayscale internally; **spot colors** for
[print/prepress](27-printing-prepress.md) are extra channels of named ink; per-channel
[adjustments](05-adjustment-layers.md) and the Channel Mixer read and write specific
planes; and classic compositing tricks (luminosity masks, edge selections from a blue
channel) are just "load a channel as a selection." This document specifies the
`ChannelSet` and `Channel` model, how it derives from color mode and
[bit depth](../glossary.md), per-channel editing/visibility, the channel↔selection
bridge, and where 16/32-bit channels arrive with [color management](15-color-management.md).

## Requirements (Functional + Non-functional)

**Functional**

- Model a document's channels as a `ChannelSet` whose **color channels are
  determined by the [color mode](01-document-system.md)**: RGB→{R,G,B}, CMYK→{C,M,Y,K},
  Gray→{Gray}, Lab→{L,a,b}; plus a single **composite alpha** channel for
  transparency.
- Support additional channels beyond color: **spot channels** (a named ink + a
  preview/solidity color, for print separations) and **saved-selection (alpha)
  channels** (named grayscale stashes).
- Store every channel as tiled grayscale data at the document's bit depth (8/16/32f),
  reusing the same [tile](../glossary.md) machinery as layer pixels and masks.
- Provide **selection→channel** ("save selection") and **channel→selection** ("load
  selection") losslessly; and **mask↔channel** equivalence (a mask is channel-like
  grayscale).
- Support **per-channel visibility** (view one or a subset of channels) and
  **per-channel editing** (paint/fill/filter a single channel, e.g. sharpen only
  Lightness, or paint into a spot channel).
- Keep color channels **derived, not independent of pixels**: editing R/G/B channels
  edits the layer/composite pixel data; spot and saved-selection channels are
  **standalone** stored planes.
- Every channel mutation (paint, save selection, add/delete spot channel, reorder) is
  a reversible [Command](21-history-undo.md) using the tile-delta mechanism.

**Non-functional**

- `pe_core`, pure C++20: no Qt. The Channels panel, per-channel thumbnails, and the
  spot-color picker are [app-shell](24-ui-workspace.md) UI.
- Channel planes participate in the [performance](22-performance.md) substrate:
  sparse, reference-counted, copy-on-write tiles, RAM-budgeted, scratch-pageable.
- The channel set must scale with **bit depth**: the same model carries 8-, 16-, and
  32-bit-float planes; depth is a property of the buffer, decoupled per channel where
  useful (a saved selection may stay 8-bit in a 16-bit document).
- 16/32-bit channels and CMYK/Lab arrive with [color management](15-color-management.md)
  in M6 (see [ADR-0004](../adr/0004-color-management.md)); the model is shaped for them
  from the start.

## Data model (concrete C++ in `namespace pe`)

Channels store single-component planes on the engine's tiles (`kTileSize`, `TileCoord`,
`Rect` from `pe/core/Tile.hpp`) — the same sparse grayscale storage the
[mask system](06-masks.md) defines.

```cpp
namespace pe {

// What a channel represents.
enum class ChannelKind : uint8_t {
    Color = 0,       // a color component (R/G/B, C/M/Y/K, Gray, or L/a/b)
    Alpha,           // the composite transparency channel
    Spot,            // a named print ink (spot color separation)
    SelectionSave,   // a stored selection (named grayscale stash)
};

// Which color component a Color channel is (interpretation depends on ColorMode).
enum class ColorComponent : uint8_t {
    None = 0,
    Red, Green, Blue,            // RGB
    Cyan, Magenta, Yellow, Black,// CMYK
    Gray,                        // Grayscale
    L, A, B,                     // Lab
};

// Precision of a channel's samples. Usually follows the document bit depth, but a
// saved-selection or spot channel may be stored shallower to save memory.
enum class ChannelDepth : uint8_t { U8 = 8, U16 = 16, F32 = 32 };

// One component plane. Color channels alias into the document's pixel storage
// (editing them edits the image); Spot and SelectionSave channels own standalone
// tiled buffers. 'previewColor' tints the channel for display and is the ink color
// for a Spot channel.
class Channel {
public:
    [[nodiscard]] ChannelKind     kind() const noexcept;
    [[nodiscard]] ColorComponent  component() const noexcept;  // for Color channels
    [[nodiscard]] const std::string& name() const noexcept;    // "Red", "PANTONE 185 C", "Alpha 1"
    [[nodiscard]] ChannelDepth    depth() const noexcept;
    [[nodiscard]] bool            visible() const noexcept;     // shown in composite/preview
    [[nodiscard]] Rgbaf           previewColor() const noexcept;// spot ink / display tint
    [[nodiscard]] float           solidity() const noexcept;    // spot ink opacity [0,1]

    // Sample coverage/value at p in [0,1]. For Color channels this reads through to
    // the pixel data; for Spot/SelectionSave it reads the standalone buffer.
    [[nodiscard]] float sample(Point p) const noexcept;

    // Standalone planes (Spot, SelectionSave) expose their grayscale buffer; Color
    // channels return the document's component view instead.
    [[nodiscard]] const MaskBuffer* standaloneBuffer() const noexcept; // null for Color
};

// The document's channels. The Color channels are fixed by ColorMode and reorder
// with it; Alpha is the composite transparency; Spot and SelectionSave channels are
// user-managed extras. Owned by the Document (Document::channels()).
class ChannelSet {
public:
    [[nodiscard]] ColorMode mode() const noexcept;             // drives the color set
    [[nodiscard]] int       colorChannelCount() const noexcept;// RGB=3, CMYK=4, Gray=1…

    [[nodiscard]] std::span<const Channel> colorChannels() const noexcept;
    [[nodiscard]] const Channel*           alpha() const noexcept;        // may be null
    [[nodiscard]] std::span<const Channel> spotChannels() const noexcept;
    [[nodiscard]] std::span<const Channel> savedSelections() const noexcept;

    [[nodiscard]] const Channel* byName(std::string_view) const noexcept;

    // Mutators run through commands (not public setters): addSpotChannel,
    // saveSelection(name), loadSelection(name, SelectionOp), deleteChannel,
    // reorderSpot, setChannelVisible, paintChannel — each a tile-delta Command.
};

} // namespace pe
```

The `colorComponentCount(ColorMode)` helper from the
[document system](01-document-system.md) drives `colorChannelCount()`. A
[mask](06-masks.md)'s `MaskBuffer` and a `SelectionSave`/`Spot` channel's
`standaloneBuffer()` are the **same grayscale tile type**, which is why
mask↔selection↔channel conversions are copies, not transforms.

## Behavior & algorithms

**Color mode determines the color set.** The `ChannelSet`'s color channels are not
free-form: they are exactly the components of the document's
[color mode](01-document-system.md). Converting mode (a
[command](01-document-system.md), via [color management](15-color-management.md))
rewrites the color channels — RGB→CMYK replaces {R,G,B} with {C,M,Y,K} and runs the
managed conversion on every layer's tiles — while **spot and saved-selection channels
are carried across unchanged** (they are standalone data, not color components).

**Color channels are a view; extras are storage.** Editing a color channel (e.g.
painting black into the Red channel) writes the corresponding component of the active
layer's pixels — the channel is a *projection* of the pixel data, so per-channel edits
and per-channel filters (sharpen Lightness only, blur a single channel) are
implemented by reading/writing one component of the `Rgbaf`/depth-typed pixels through
the same tile pipeline. **Spot** and **SelectionSave** channels own independent
grayscale buffers and are edited directly.

**Selection ↔ channel (save/load).** "Save selection" snapshots the document's
[`SelectionMask`](07-selection-system.md) coverage into a new `SelectionSave` channel:

```
saveSelection(doc, name):
    ch = new Channel{kind=SelectionSave, name, depth=doc.depth}
    copy selection.mask().buffer()  ->  ch.standaloneBuffer()   # grayscale copy
    channelSet.add(ch)                                          # as a command

loadSelection(doc, name, op):                                  # op ∈ SelectionOp
    region = channelSet.byName(name).standaloneBuffer()
    newMask = Selection::combine(selection.mask().buffer(), region, op)
    set document selection = newMask                            # as a command
```

Both directions are lossless at equal precision because the selection mask, layer
masks, and these channels are the same tiled grayscale format. **Channel→selection**
also works for *color* channels (load the Blue channel as a selection to get a classic
high-contrast edge mask) by reading the component plane as coverage.

**Mask ↔ channel.** A [mask](06-masks.md) is channel-like grayscale; "save mask as
channel" and "make mask from channel" are copies between a layer's `MaskBuffer` and a
`SelectionSave`/spot channel. This is why the [file format](20-file-io.md) stores all
three with one tiled-grayscale serializer.

**Spot channels.** A spot channel names a print ink (e.g. a Pantone), stores its
**solidity** and a **preview color** for on-screen visualization, and holds a grayscale
plane of ink coverage (where, and how much, the ink prints). Spot channels are
**additive on top of** the process color separation, ride through mode changes, and are
consumed by [print/prepress](27-printing-prepress.md) when generating separations or a
PDF. On screen they are simulated by tinting their coverage with `previewColor` at
`solidity` over the composite (a soft-proof-style approximation, not the composite's
working color).

**Per-channel visibility.** Toggling a channel's `visible()` changes what the
[canvas](02-canvas-rendering.md) displays: viewing a single color channel shows it as
grayscale (or tinted); hiding the composite color and showing only a spot channel
previews that separation. Visibility is display state (a `DocumentChange`, not a pixel
edit) and does not alter stored data.

**Bit depth.** Each channel carries a `ChannelDepth`. Color channels follow the
document depth (8 at M1; 16/32f at M6); spot and saved-selection channels may stay
8-bit to save memory even in a deeper document (precision is per-buffer). All depths
flow through the same tile storage and the same paging/budget policy.

## Interactions

- **[Document system](01-document-system.md)** — the `ChannelSet` is owned by the
  document; its color channels are fixed by `colorMode()` and rewritten by
  mode-conversion commands; depth tracks `bitDepth()`.
- **[Color management](15-color-management.md)** — owns mode/profile conversions that
  rewrite color channels; 16/32-bit and CMYK/Lab channels arrive here in M6
  ([ADR-0004](../adr/0004-color-management.md)).
- **[Selection system](07-selection-system.md)** — save/load selection moves coverage
  to/from `SelectionSave` channels; any channel can be loaded as a selection.
- **[Masks](06-masks.md)** — masks are channel-like grayscale; mask↔channel are
  copies on the shared `MaskBuffer` type.
- **[Adjustment layers](05-adjustment-layers.md)** — per-channel adjustments
  (Levels/Curves/Channel Mixer) read/write specific color channels; the available
  channels depend on the mode this system defines.
- **[Printing / prepress](27-printing-prepress.md)** — spot channels become print
  separations; solidity/preview color drive ink simulation and PDF/print output.
- **[Canvas & rendering](02-canvas-rendering.md)** — per-channel visibility selects
  what the compositor/display path shows (single channel, spot preview, composite).
- **[File I/O](20-file-io.md)** — alpha, spot, and saved-selection channels (names,
  depths, spot ink color/solidity) are persisted in the native format and mapped to
  PSD channels where structure exists.
- **[Command/history](21-history-undo.md)** — paint-channel, save/load selection, and
  add/delete/reorder spot channel are tile-delta or structural commands.
- **UI touchpoints (app shell)** — the Channels panel (per-channel thumbnails,
  visibility eyes, "load channel as selection", "save selection as channel", new spot
  channel dialog, ink color/solidity picker) is Qt UI; the engine exposes the
  `ChannelSet`, the buffers, and the commands.

## Performance, threading & GPU

- Channel planes are stored, reference-counted, paged, and threaded exactly like
  layer pixel and mask tiles ([performance](22-performance.md)); a spot or saved
  channel costs memory only where it has content.
- Color-channel reads/writes are a strided view into existing pixel tiles — no extra
  storage; per-channel filters run the same tile jobs on one component.
- Save/load selection and mask↔channel are tile copies (copy-on-write where the source
  is immutable), not conversions — O(touched tiles).
- Spot-channel display simulation is a per-tile tint+composite, foldable into the
  display pass on the [GPU/RHI](23-gpu-acceleration.md) path; it never alters stored
  composite data.
- Channel operations partition by tile across the worker pool with no aliasing, like
  the rest of the engine.

## Edge cases & failure modes

- **Mode without alpha** (e.g. a flattened Bitmap/Indexed export) — `alpha()` returns
  null; coverage is implicit; the model tolerates an absent alpha channel.
- **Mode change carrying extras** — RGB→CMYK rewrites color channels but must preserve
  spot and saved-selection channels untouched; a round-trip back to RGB restores the
  color set (color data may be lossy per the conversion, extras are not).
- **Spot channel in a screen-only document** — allowed; it is print metadata,
  previewed via simulation; ignored by flat raster export that can't carry it (with a
  warning), preserved by the native format and PSD/PDF.
- **Loading a color channel as a selection** — reads the component plane as coverage;
  high/low values map to selected/unselected (classic luminosity/edge masks); never
  modifies the source channel.
- **Depth mismatch on load** (16-bit channel into an 8-bit selection) — resampled to
  the target precision; documented as potentially lossy downward, exact upward.
- **Deleting a color channel** — disallowed (color channels are fixed by mode); only
  spot and saved-selection channels are deletable. UI enforces; the model rejects.
- **Empty saved selection** — a fully-black `SelectionSave` channel is valid (selects
  nothing on load); not an error.
- **Many saved channels** — each is sparse; an all-default channel costs near zero;
  the format stores only non-default tiles.

## Testing strategy

Headless `pe_core` unit + golden tests:

- **Mode→channel mapping** — `ChannelSet` for each `ColorMode` has the right color
  channels and count; mode conversion rewrites color channels and preserves spot and
  saved-selection channels.
- **Selection round-trip** — `selection → save channel → load channel` reproduces the
  exact coverage; with `add/subtract/intersect` ops the combine matches the
  [selection](07-selection-system.md) algebra.
- **Mask↔channel** — a layer mask copied to a channel and back is byte-exact at equal
  precision.
- **Per-channel edit** — painting/filtering a single color channel changes only that
  component of the pixels (pixel-probe asserts on the others).
- **Channel→selection from color** — loading the Blue channel yields coverage equal to
  the channel's normalized values (edge/luminosity mask correctness).
- **Spot channel** — add/delete/reorder commands round-trip; ink color/solidity and
  coverage persist through the native format; display simulation matches a golden tint.
- **Depth** — 8/16/32f channel buffers store and sample correctly; a shallow saved
  channel in a deep document is allowed and preserved.
- **Undo** — every channel command (paint, save/load, add/delete spot) restores prior
  state exactly.

## Phasing

- **M4 (precursor)** — saved-selection channels exist implicitly via
  [selection](07-selection-system.md) save/load on the shared grayscale buffer; the
  default R/G/B/A set is the document's implicit channels.
- **M6 (this doc lands as a surfaced system)** — the full `ChannelSet`/`Channel` model;
  per-channel visibility/editing; spot channels; **16-bit and 32-bit-float** channel
  paths and **CMYK/Lab** color sets arriving with
  [color management](15-color-management.md) ([ADR-0004](../adr/0004-color-management.md)).
- **M7** — alpha, spot, and saved-selection channels round-trip through the native
  format and map to/from PSD channels.
- **M10** — spot channels drive [print/prepress](27-printing-prepress.md) separations,
  ink simulation, and PDF export.

## Open questions

- Spot-color **overprint/knockout** semantics and accurate on-screen ink simulation —
  how faithful do we get before M10 print work, and does it need a spectral model?
- Should saved-selection channels default to the document depth or always 8-bit (to
  save memory), with an opt-in to higher precision?
- Maximum practical channel count (many spot inks + many saved selections) and whether
  the Channels panel/format needs pagination or grouping.
- Do we expose Lab channels for direct editing (Lightness sharpening is a common pro
  trick) in M6, or defer the Lab editing UI to a later pass?

## References

- [01 — Document system](01-document-system.md) — the `ChannelSet` on the document;
  color mode drives the channel set; depth.
- [15 — Color management](15-color-management.md) — mode/profile conversions, 16/32-bit
  and CMYK/Lab; [ADR-0004](../adr/0004-color-management.md).
- [06 — Masks](06-masks.md) — masks as channel-like grayscale; mask↔channel.
- [07 — Selection system](07-selection-system.md) — save/load selection, channel↔
  selection, the boolean `combine`.
- [27 — Printing / prepress](27-printing-prepress.md) — spot channels and separations.
- [05 — Adjustment layers](05-adjustment-layers.md) — per-channel adjustments.
- [20 — File I/O](20-file-io.md) — channel persistence (native + PSD).
- [02 — Canvas & rendering](02-canvas-rendering.md) — per-channel visibility/display.
- [Master architecture](../01-master-architecture.md) · [Glossary](../glossary.md)
  (Channel, Selection, Mask, Bit depth).
