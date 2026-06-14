# ADR-0007 — A versioned native document format; PSD for interchange

**Status:** Accepted

## Context

The document model is richer than any interchange format captures perfectly:
layer tree, masks, channels, paths, smart objects, adjustment parameters,
metadata, color profile, and (optionally) history. We need lossless persistence of
*our* model, plus interoperability with the wider world — above all Photoshop's
PSD/PSB.

Trying to use PSD as our working format would shackle the engine to Photoshop's
legacy record layout and its quirks, and still wouldn't represent features PSD
lacks. Conversely, ignoring PSD would isolate us from real-world assets.

## Decision

Maintain **two distinct format responsibilities**:

1. A **native, versioned document format** that round-trips the full PhotoEdit
   model losslessly. It is tile-aware (stores tiled, compressed pixel data),
   carries an explicit **format version** independent of the app version, and is
   designed for forward/backward-compatible evolution.
2. **PSD/PSB as an interchange format**: faithful import/export of the *common,
   important* structure (layers, groups, masks, basic adjustments, text where
   feasible, profiles). We explicitly do **not** target bit-for-bit parity with
   every exotic legacy PSD record.

Flat raster formats (PNG/JPEG/TIFF/WebP/…) are export/flatten targets, not
working formats.

## Consequences

**Positive**
- Users never lose PhotoEdit-specific structure in their own files.
- We control format evolution and can store new model concepts cleanly.
- PSD interop covers real-world interchange without contorting the engine.

**Negative / costs**
- Two writers/readers to maintain (native + PSD), plus the raster exporters.
- PSD's complexity means ongoing "faithful-enough" judgment calls and a test
  corpus of real files.
- Format versioning discipline (migration, compatibility tests) is required.

## Alternatives considered

- **Use PSD/PSB as the native format.** Maximum compatibility, but couples us to
  Adobe's legacy layout and can't represent non-PSD features; rejected.
- **Only support PSD + flat exports (no native format).** Would lose model
  fidelity on save; rejected.
- **A generic container (e.g. SQLite/zip-of-parts).** The native format may well
  be implemented as a structured container internally; that's an implementation
  detail of this decision, not an alternative to it.

## Notes

Specifics — chunk layout, compression, versioning/migration, and the PSD mapping
table — are in [systems/20-file-io.md](../systems/20-file-io.md). The format
version is independent of `pe::Version`.
