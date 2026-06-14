# 20 — File I/O

> Milestone: M7 · Status: Spec

## Purpose

This system is PhotoEdit's door to the outside world: it turns files on disk into
[`Document`](01-document-system.md)s and turns documents back into files. The hard
constraint that shapes everything here is that **the document model is richer than
any interchange format captures perfectly** — a tree of pixel/text/shape/
adjustment/smart-object layers, masks, channels, paths, smart filters, a color
profile, and metadata. So we maintain two distinct responsibilities
([ADR-0007](../adr/0007-native-document-format.md)):

1. A **native, versioned format** that round-trips the *entire* model losslessly.
   It is tile-aware, carries a format version independent of `pe::Version`, and is
   built to evolve forward/backward-compatibly.
2. **Everything else** — PSD/PSB for interchange, and the flat raster/exchange
   formats (PNG, JPEG, WebP, TIFF, GIF, BMP, PDF, TGA, EXR) — each of which can
   represent only *part* of the model, so the exporter must **flatten, preserve,
   compress, convert, or discard** structure per the format's rules.

The discipline that keeps this tractable: every format is a pluggable
`ImageImporter`/`ImageExporter` behind a registry, so PSD and PNG are *clients* of
the same interface a [plugin](29-plugin-extension.md) would use. Decoding and
encoding are heavy and run **off the document thread**; results merge back as
[commands](../glossary.md)/observer events. The native format and the engine model
are the source of truth — a format never dictates the model, it serves it.

## Requirements

**Functional**

- **Operations:** open, save, save as, save a copy, export, quick export, export
  for web, batch export, asset export, local save, and cloud-document save. Each
  resolves to an importer or exporter plus a target (path, stream, or cloud
  handle).
- **Formats (read/write):** the **native PhotoEdit format** (`.peb`), **PSD**,
  **PSB**, **PNG**, **JPEG**, **WebP**, **TIFF**, **GIF**, **BMP**, **PDF**,
  **TGA**, **EXR**. Each obeys its own rules (the table below is normative):

  | Format | Layers | Depth | Alpha | Lossy | Notes |
  | --- | --- | --- | --- | --- | --- |
  | Native `.peb` | full tree | 8/16/32f | yes | no | full-fidelity round-trip; versioned |
  | PSD | common tree | 8/16/32 | yes | no | interchange, not bit-parity; ≤30k px |
  | PSB | common tree | 8/16/32 | yes | no | like PSD for **huge** docs (>30k px) |
  | PNG | flatten | 8/16 | yes | no | lossless; transparency |
  | JPEG | flatten | 8 | **no** | yes | lossy; quality knob; no layers/alpha |
  | WebP | flatten | 8 | yes | both | web compression (lossy or lossless) |
  | TIFF | optional layers | 8/16/32 | yes | no* | print/archive; can store layers in a private tag |
  | GIF | flatten | indexed | 1-bit | no | ≤256 colors; animation frames |
  | BMP | flatten | 8 | optional | no | simple uncompressed raster |
  | PDF | flatten/raster | 8/16 | no | both | page; native model embeddable for re-open |
  | TGA | flatten | 8 | yes | no | RLE-optional raster interchange |
  | EXR | flatten/channels | **32f**/16f | yes | no | HDR, scene-linear, named channels |

  *TIFF supports several compressions (LZW/ZIP/none) and is lossless in practice.

- The **native format must round-trip the full model with zero loss** (layers,
  groups, masks, channels, paths, smart objects + their sources and smart filters,
  adjustment parameters, blend/opacity/effects, guides/artboards, color profile,
  metadata, and optionally history).
- **PSD/PSB** import/export is **faithful interchange of common structure**, not
  bit-for-bit parity: layers, groups, masks, basic adjustments, text where
  feasible, the profile. Unrepresentable structure degrades predictably.
- The exporter, per target format, must **flatten** (collapse the tree),
  **preserve** (keep structure the format supports), **compress** (per the
  format's codec), **convert** (color mode/bit depth/profile), or **discard**
  (drop what cannot be represented) — and surface what was lost.
- **Color-profile embedding** on export and **profile extraction** on import;
  **metadata (EXIF/XMP/IPTC)** preserved verbatim across round-trips where a field
  isn't explicitly modeled.
- Pluggable formats: third parties register importers/exporters via the same
  interface ([plugins](29-plugin-extension.md)).

**Non-functional**

- `pe_core`, pure C++20: codecs and the native (de)serializer have **no Qt**
  ([ADR-0006](../adr/0006-headless-core-separation.md)). The shell owns file
  dialogs and progress UI ([UI / workspace](24-ui-workspace.md)).
- I/O runs **off the document thread** on the worker pool / I/O tasks
  ([performance](22-performance.md), [master architecture threading](../01-master-architecture.md#threading-model));
  the UI never blocks on a large save/open.
- **PSB / native** must stream documents **larger than RAM**: read/write
  tile-by-tile, never materializing the whole image
  ([ADR-0003](../adr/0003-tile-based-engine.md)).
- Reading an untrusted file must be **robust**: bounds-checked, fuzz-tested, and
  failing cleanly (a corrupt file is an error, never a crash).
- Saving is **crash-safe**: write to a temp file and atomically rename; never
  truncate the user's existing file on a failed save.

## Data model

Concrete, illustrative shapes in `namespace pe`. The codecs depend only on engine
model types; UI types never appear.

```cpp
namespace pe {

enum class Format : uint8_t {
    Native, Psd, Psb, Png, Jpeg, WebP, Tiff, Gif, Bmp, Pdf, Tga, Exr
};

// What the user is doing — picks defaults (flatten? embed profile? layers?).
enum class SaveIntent : uint8_t {
    Save,         // overwrite current, native format preferred
    SaveAs,       // new path/format, becomes the document's path
    SaveACopy,    // write a copy; document keeps its current path/dirty state
    Export,       // produce a deliverable (usually flattened raster)
    QuickExport,  // one-click export using the saved default preset
    ExportForWeb, // size-optimized, sRGB, stripped metadata by default
    AssetExport,  // per-layer/-artboard slices to a folder
    CloudSave,    // native model to the cloud document store
};

// Result of probing bytes/extension: which decoder to use and a quick summary.
struct FormatProbe {
    Format format;
    bool   hasLayers = false;
    bool   isAnimated = false;
    Size   canvas{};
    BitDepth depth = BitDepth::U8;
    ColorMode mode = ColorMode::RGB;
};

// Options handed to an exporter; only the relevant fields are read per format.
struct ExportOptions {
    Format     format = Format::Png;
    BitDepth   depth  = BitDepth::U8;      // converted to if the format requires
    ColorMode  mode   = ColorMode::RGB;
    bool       flatten = true;             // ignore for native/PSD-with-layers
    int        quality = 90;               // JPEG/WebP lossy [0..100]
    bool       lossless = false;           // WebP
    bool       embedProfile = true;        // ICC into the file
    bool       includeMetadata = true;     // EXIF/XMP
    bool       interlaced = false;         // PNG/GIF
    const ColorProfile* outputProfile = nullptr; // convert into before encode
};

// Pluggable decode. Streams so PSB/native never load the whole image.
class ImageImporter {
public:
    virtual ~ImageImporter() = default;
    virtual std::span<const Format> formats() const = 0;
    virtual bool canDecode(std::span<const std::byte> header) const = 0;
    virtual FormatProbe probe(ReadStream&) = 0;
    // Build a Document (tiles faulted lazily for huge files), reporting progress.
    virtual Result<std::unique_ptr<Document>> read(ReadStream&, ImportReport&,
                                                   ProgressSink&) = 0;
};

// Pluggable encode. The exporter performs flatten/convert/compress/discard.
class ImageExporter {
public:
    virtual ~ImageExporter() = default;
    virtual std::span<const Format> formats() const = 0;
    virtual ExportCapabilities capabilities(Format) const = 0; // layers? alpha? depths?
    virtual Result<void> write(const Document&, const ExportOptions&,
                               WriteStream&, ExportReport&, ProgressSink&) = 0;
};

// Registry: built-ins and plugins register here; open/save look up by probe/ext.
class FormatRegistry {
public:
    void registerImporter(std::shared_ptr<ImageImporter>);
    void registerExporter(std::shared_ptr<ImageExporter>);
    ImageImporter* importerFor(std::span<const std::byte> header) const;
    ImageExporter* exporterFor(Format) const;
};

} // namespace pe
```

**The native format container.** A chunked, versioned layout (the structured
container ADR-0007 anticipates). Conceptually:

```cpp
// File = header + a sequence of typed, length-prefixed chunks. Unknown chunks are
// skipped on read, so a newer file opens (degraded) in an older build, and an
// older file opens fully in a newer one.
struct PebHeader {
    char     magic[4] = {'P','E','B','1'};
    uint16_t formatMajor;     // bumped on incompatible change; INDEPENDENT of pe::Version
    uint16_t formatMinor;     // bumped on additive change
    uint32_t flags;           // e.g. historyIncluded, bigOffsets (PSB-scale)
};

enum class ChunkId : uint32_t {
    DocumentInfo, ColorProfile, Metadata, LayerTree, LayerTiles, Mask, Channel,
    Path, SmartObjectSource, SmartFilterStack, Guides, Artboards, Thumbnail,
    History /* optional */, End
};
// LayerTiles stores per-tile, per-layer compressed blocks keyed by TileCoord, so
// reads/writes are streamable and only populated tiles cost bytes.
```

## Behavior & algorithms

**Open (the import path).**

```
open(path):
    bytes = readHeader(path, N)                 # a few KB
    imp   = registry.importerFor(bytes)         # by magic, then extension
    if !imp: fail("unsupported format")
    stream = openRead(path)                      # off the document thread
    doc = imp.read(stream, report, progress)     # tiles faulted lazily
    doc.setPath(path); doc.attachProfile_or_assign()
    mergeOntoDocumentThread(doc)                 # becomes the active document
    surfaceImportReport(report)                  # "text rasterized", "CMYK→RGB", …
```

The importer converts inputs **into the working color space** at the document's
chosen depth ([color management](15-color-management.md)): an sRGB JPEG, a
scene-linear EXR, and a CMYK TIFF all land in a coherent working document. If a
file embeds no profile, we assign a sensible default (sRGB for 8-bit display
formats) and record that assumption.

**Save vs. export (the two families).** *Save* persists the **model** (native, or
PSD/PSB interchange). *Export* produces a **deliverable** (usually flattened
raster). The exporter runs a fixed pipeline per format capabilities:

```
export(doc, opts):
    img = opts.flatten ? compositeFlattened(doc)      # full composite → one raster
                       : doc                            # native/layered TIFF/PSD
    img = convertColor(img, opts.mode, opts.depth, opts.outputProfile)  # CONVERT
    img = adaptStructure(img, exporter.capabilities)   # DISCARD/PRESERVE per format
    bytes = encode(img, opts)                           # COMPRESS per codec
    atomicWrite(target, bytes)                          # temp + rename
    report := what was discarded/converted (alpha dropped? 16→8? text flattened?)
```

`adaptStructure` is where the per-format rules bite: dropping alpha for JPEG,
quantizing to ≤256 colors for GIF, collapsing layers for PNG/BMP, splitting named
channels for EXR, keeping layers for native/PSD/layered-TIFF.

**The native format (`.peb`) round-trip.** Write each model part as a chunk;
`LayerTiles` walks each pixel layer's populated tiles and writes a compressed
block per `TileCoord` (so a 30k×30k document with sparse content stays small and
streams). Read reverses it, faulting tiles lazily so opening a huge document does
**not** allocate its pixels. The format version is checked first: an additive
(minor) bump still loads in older builds (unknown chunks skipped); a major bump is
gated with a clear message and, where possible, a one-way **migration**.

**PSD/PSB mapping.** A bidirectional mapping table translates between our model and
Photoshop's records, accepting deliberate lossiness:

```
to our model (import)            from our model (export)
─────────────────────────        ─────────────────────────
layer/group records   → Layer/Group    Layer/Group → layer/group records
layer & vector masks  → Mask           Mask        → mask records
basic adjustments     → Adjustment     Adjustment  → corresponding adj. layer
text (TySh) where feasible → text layer (else rasterized faithfully)
blend mode / opacity  → BlendMode/opacity   (1:1 on the shared set)
ICC profile           → ColorProfile   ColorProfile → ICC profile
unknown/exotic records → preserved as opaque "passthrough" blocks where possible
```

PSB is the **same mapping with 64-bit offsets/dimensions** for >30k-px documents;
the importer/exporter share code and branch on the size class. We never claim to
reproduce every legacy record bit-for-bit ([vision: out of scope](../00-vision-and-scope.md#out-of-scope-at-least-initially));
the report tells the user precisely what changed.

**Format-specific encode rules (selected).**

- **JPEG:** flatten, drop alpha (composite over a chosen matte), 8-bit only,
  quality-controlled DCT; embed profile + EXIF unless export-for-web strips them.
- **PNG:** flatten, keep alpha, 8/16-bit, lossless DEFLATE; optional interlacing.
- **GIF:** flatten, **quantize to an indexed palette** (≤256, with dithering
  options), 1-bit transparency; **animation** writes a frame per source frame with
  per-frame delays and disposal.
- **TIFF:** archival; 8/16/32-bit, LZW/ZIP/none; may **preserve layers** in a
  private tag while remaining a valid flat TIFF for other readers.
- **WebP:** flatten; lossy (quality) or lossless; alpha supported; tuned for web
  size.
- **EXR:** scene-linear **32-bit float** (or half), named channels (RGBA + extras),
  PIZ/ZIP compression; the path that preserves HDR range
  ([color management](15-color-management.md)).
- **PDF:** a page; raster export by default, with the option to **embed the native
  model** so a PhotoEdit-saved PDF re-opens with full structure (prepress detail in
  [printing/prepress](27-printing-prepress.md)).
- **BMP / TGA:** straightforward rasters for legacy/interchange (TGA optionally
  RLE-compressed, alpha-capable).

**Batch & asset export.** *Batch export* runs the export pipeline across many
documents/presets as a queue on the worker pool, reusing the same `ImageExporter`
([automation](26-automation.md)). *Asset export* slices a document **per layer or
artboard** (named by layer/artboard) into a folder, each through the exporter with
its own scale/format. Both are "many small exports," so they reuse one path.

**Cloud vs. local save.** *Local save* writes a `.peb` (or chosen format) to the
filesystem. *Cloud-document save* serializes the **same native model** to the
cloud store ([cloud / account](28-cloud-account.md)) — possibly chunk-delta'd for
sync — but the bytes are the native format; the cloud is a transport, not a second
format.

## Interactions

- **[Document system](01-document-system.md):** import builds a `Document`; save
  reads its full state. Open path, dirty flag, and save-format hint live there.
- **[Layer system](03-layer-system.md):** the native writer serializes the layer
  tree and each kind; the PSD mapper translates layer kinds to/from PSD records.
- **[Color management](15-color-management.md):** import converts into the working
  space; export converts to the output profile and embeds ICC; EXR carries
  scene-linear.
- **[Performance](22-performance.md):** I/O runs on worker/I/O tasks; native/PSB
  stream tile-by-tile through the same tile/compression machinery; previews/
  thumbnails reuse the tile cache.
- **[Master architecture](../01-master-architecture.md#persistence):** this system
  *is* the Persistence box; it sits beside automation and the plugin host.
- **[Plugin / extension](29-plugin-extension.md):** importers/exporters are the
  pluggable surface; built-ins use the identical interface.
- **[Automation](26-automation.md):** batch/asset/quick export are scriptable and
  recordable as command sequences.
- **[Metadata in the document](01-document-system.md):** EXIF/XMP preserved
  verbatim through the model's `Metadata` block.

## Performance, threading & GPU

- **Off the document thread:** decode/encode and scratch I/O run on worker/I/O
  tasks; the document thread only merges the finished `Document` or marks
  saved/clean. Progress is reported back for the UI.
- **Streaming, not buffering:** native and PSB read/write **per tile**, so peak
  memory is bounded by the working set, not the document size
  ([ADR-0003](../adr/0003-tile-based-engine.md)). Export of a huge flattened raster
  composites and encodes in tile bands.
- **Compression** (DEFLATE/LZW/PIZ) is CPU-bound and parallelizes per tile/band on
  the worker pool; the same per-tile compression backs scratch-disk paging in
  [performance](22-performance.md).
- **GPU:** I/O is fundamentally a CPU/disk activity (decode, compress, byte
  layout). The GPU is *not* on the save/open path; thumbnails may be generated by
  reusing already-composited [display tiles](23-gpu-acceleration.md), but the
  encoders read CPU pixels.
- **Quick export** caches its last preset so the common case is one click → one
  background encode.

## Edge cases & failure modes

- **Corrupt / truncated file:** decoders are bounds-checked and fuzz-tested; a bad
  file yields a clean error, never a crash or partial mutation of the open
  document.
- **Format can't hold the model:** export proceeds but the `ExportReport` lists
  every loss (alpha dropped, 16→8 quantize, layers flattened, text rasterized,
  out-of-gamut clipped). Save-as to native warns *only* when the user picked a
  lossy target.
- **>30k px to PSD:** rejected with guidance to use **PSB**; the exporter detects
  the size class and suggests/escalates.
- **Disk full / write error mid-save:** the temp-file + atomic-rename strategy
  leaves the original intact; the operation reports failure.
- **Profile mismatch / missing profile:** import assigns a default and records the
  assumption; export without an embedded profile is allowed but flagged.
- **Indexed/animation specifics:** GIF beyond 256 colors quantizes (with chosen
  dithering); non-animated export of an animated source takes the current frame.
- **EXR/HDR into an 8-bit format:** requires tone-mapping/clipping; the exporter
  refuses to silently destroy HDR range and asks for an explicit conversion.
- **Native format version newer than the app:** open is gated with a clear message;
  known additive bumps still load (unknown chunks skipped).
- **Smart objects / linked sources:** embedded sources serialize into `.peb`;
  linked sources store a path + checksum and re-link (or warn) on open.

## Testing strategy

- **Native round-trip (the headline test):** build a document exercising every
  model feature (all layer kinds, masks, channels, paths, smart objects + smart
  filters, 8/16/32-bit, profile, metadata), write `.peb`, read back, and assert
  **structural and pixel equality** — zero loss.
- **Streaming / huge documents:** write and re-read a sparse 30k×30k `.peb` (and a
  PSB) with a capped RAM budget; assert peak memory stays bounded and tiles fault
  lazily.
- **Format-rule tests:** per format, assert capabilities are honored — JPEG drops
  alpha and rejects 16-bit; GIF quantizes to ≤256; PNG keeps alpha lossless; EXR
  preserves 32f range; TIFF layered-tag round-trips and still reads as flat
  elsewhere.
- **PSD/PSB interchange:** a corpus of real-world PSD/PSB files imports and
  composites correctly (golden images via the [compositor](02-canvas-rendering.md));
  export → re-import preserves the common structure within documented fidelity.
- **Color/metadata fidelity:** profiles embed and extract; EXIF/XMP survive a
  round-trip verbatim; color converts correctly on import/export against
  references.
- **Robustness:** fuzz every decoder (truncated/corrupt/hostile inputs) under
  ASan/UBSan; assert no crash and clean errors.
- **Crash-safety:** simulated write failures leave the prior file intact.

## Phasing

- **M6 (prerequisite):** color management lands first so saved/loaded files carry
  correct profiles ([roadmap rationale](../03-roadmap-and-milestones.md#sequencing-rationale)).
- **M7 (this doc lands):** native format (full round-trip, versioned); PSD/PSB
  import/export (common structure); PNG/JPEG/TIFF/WebP/GIF/BMP read/write;
  export-for-web and quick export; metadata + profile preservation. TGA/EXR land
  here or alongside as the raster set is completed.
- **M8:** smart-object/text fidelity in native and PSD deepens as those systems
  ship ([smart objects](11-smart-objects.md), [text](17-text-typography.md)).
- **M10:** batch/asset export wired into [automation](26-automation.md); **PDF**
  export matured with [printing/prepress](27-printing-prepress.md); cloud-document
  save via [cloud/account](28-cloud-account.md); third-party format
  [plugins](29-plugin-extension.md).

## Open questions

- **Native container substrate:** a fully bespoke chunk format vs. a structured
  container (zip-of-parts / SQLite) under the same chunk semantics — ADR-0007 calls
  this an implementation detail; we'll settle it with streaming/perf data.
- **History in `.peb`:** optional today; what's the default, and how do we bound the
  size when included?
- **PSD passthrough blocks:** how much exotic structure do we round-trip opaquely
  vs. drop with a report?
- **PDF scope:** how far toward true vector/print PDF (vs. raster-in-PDF) do we go,
  and where does [printing/prepress](27-printing-prepress.md) take over?
- **Linked smart-object resolution:** re-link UX and checksum policy when a linked
  source moved or changed.

## References

- [ADR-0007 — native document format; PSD for interchange](../adr/0007-native-document-format.md)
  — the two-format decision this spec implements.
- [01 — Master Architecture](../01-master-architecture.md#persistence) — where File
  I/O sits; the model it serializes.
- [00 — Vision & Scope](../00-vision-and-scope.md#out-of-scope-at-least-initially)
  — "faithful interchange, not bit-for-bit PSD parity."
- [Glossary](../glossary.md) — Document, Tile, PSD/PSB, Non-destructive.
- ADRs: [0003 — tile-based engine](../adr/0003-tile-based-engine.md),
  [0006 — headless core](../adr/0006-headless-core-separation.md).
- Sibling systems: [document](01-document-system.md), [layers](03-layer-system.md),
  [color management](15-color-management.md), [performance](22-performance.md),
  [GPU acceleration](23-gpu-acceleration.md), [automation](26-automation.md),
  [printing/prepress](27-printing-prepress.md), [cloud/account](28-cloud-account.md),
  [plugin/extension](29-plugin-extension.md).
