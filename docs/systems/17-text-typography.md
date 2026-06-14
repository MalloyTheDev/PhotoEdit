# 17 — Text & Typography

> Milestone: M8 · Status: Spec

## Purpose

A text layer is editable *data*, not pixels: a string with styling that the
[compositor](02-canvas-rendering.md) shapes, lays out, and rasterizes live into
the working [color space](15-color-management.md). The text engine covers font
selection, sizing, kerning/tracking/leading, paragraphs, OpenType features, glyph
rendering (including emoji and right-to-left), warping, and anti-aliasing — so
type stays re-editable at any time and crisp at any zoom until rasterized.

## Requirements

**Functional**

- Point text (anchor) and paragraph text (a bounded box that wraps).
- Character styling per run: font family/style/weight, size, color, kerning,
  tracking, leading, baseline shift, faux bold/italic, underline/strikethrough.
- Paragraph styling: alignment/justification, indents, space before/after,
  hyphenation.
- OpenType features (ligatures, alternates, small caps), emoji/color fonts,
  bidirectional (RTL) and complex-script shaping.
- Text warp (arc, wave, etc.) as a mesh deformation of the rasterized type.
- Convert to shape/path; edit text anytime without quality loss.

**Non-functional**

- `pe_core` owns the text model and produces engine pixels; the editing caret/IME
  UI lives in the [app shell](24-ui-workspace.md).
- Live re-rasterization is fast enough for interactive typing.

## Data model

```cpp
namespace pe {

struct CharStyle {
    std::string fontFamily, fontStyle;     // resolved to a face id
    float sizePt = 12;
    Rgbaf color{0,0,0,1};
    float tracking = 0, kerning = 0, baselineShift = 0;
    bool underline = false, strikethrough = false;
    OpenTypeFeatures features;
};

struct ParagraphStyle {
    enum Align { Left, Center, Right, JustifyLast, JustifyAll } align = Left;
    float leading = 0;            // 0 = auto
    float indentStart = 0, indentEnd = 0, spaceBefore = 0, spaceAfter = 0;
    bool hyphenate = false;
    TextDirection direction = TextDirection::Auto;  // LTR/RTL/auto
};

struct StyledText {
    std::u16string text;                  // UTF-16 storage
    std::vector<StyleRun<CharStyle>> runs;
    std::vector<Paragraph> paragraphs;    // with ParagraphStyle
};

struct TextLayer {                        // a Layer kind
    StyledText content;
    enum { Point, Paragraph } mode;
    RectF box;                            // for paragraph mode
    Transform transform;
    TextWarp warp;                        // optional
    // rasterized preview cache (tiled), invalidated on edit
};

} // namespace pe
```

## Behavior & algorithms

Shaping → layout → rasterize:

```
1. Itemize the string into runs by script/direction/style (bidi resolution).
2. Shape each run with the font (HarfBuzz): glyph ids + positions + clusters,
   applying kerning and OpenType features.
3. Lay out lines: break (and hyphenate/justify) within the box, apply leading and
   alignment, compute glyph pen positions.
4. Rasterize glyph outlines (FreeType) with anti-aliasing into the layer's tiles,
   in the working color space; cache the result.
5. Apply text warp as a mesh transform on the rasterized type if present.
```

Editing any character/style invalidates the affected lines and re-runs from the
needed stage. Glyph outlines feed "convert to shape" → [vector paths](18-vector-paths.md).

We use a real shaping/rasterization stack (HarfBuzz + FreeType, or the platform/Qt
text stack) but keep the output as engine pixels so the compositor stays toolkit-
independent (see [ADR-0006](../adr/0006-headless-core-separation.md)).

## Interactions

- [Layer system](03-layer-system.md): `TextLayer` is a layer kind with live raster.
- [Vector/paths](18-vector-paths.md): convert-to-shape; type on a path.
- [Transform](10-transform-system.md): warp and layer transform.
- [Color management](15-color-management.md): glyph color in the working space.
- [File I/O](20-file-io.md): native format stores editable text; PSD text mapping
  is best-effort.

## Performance, threading & GPU

- Shaping/layout are CPU and fast; rasterization caches per line/tile.
- Large blocks rasterize on [workers](22-performance.md); only edited lines redraw.
- GPU glyph atlas is a possible later optimization.

## Edge cases & failure modes

- Missing glyph → font fallback chain; tofu only as last resort.
- Mixed-direction (bidi) runs → correct reordering, caret mapping.
- Color/emoji fonts (COLR/CBDT) → render as bitmaps/layers.
- Very small sizes → hinting/AA quality; subpixel positioning.
- Font not installed on open → substitute + flag (preserve original name).

## Testing strategy

- Unit: run itemization, bidi level assignment, line-break positions for known
  inputs; style-run splitting/merging.
- Golden-image: render reference strings (Latin, RTL, ligatures, emoji) and
  compare within tolerance.
- Round-trip: text + styles survive native save/load unchanged.

## Phasing

- **M8 early**: point/paragraph text, single-font styling, alignment, AA raster,
  layer transform.
- **M8 later**: full per-run styling, OpenType, bidi/complex scripts, warp,
  type-on-path, convert-to-shape.

## Open questions

- Shaping stack: bundle HarfBuzz/FreeType vs use Qt's text engine in the shell and
  pass rasters down (tension with headless core).
- Font management/embedding in the native format.

## References (relative links)

- [Glossary](../glossary.md) — Layer, Working space.
- Sibling systems: [03 — Layer system](03-layer-system.md),
  [18 — Vector/paths](18-vector-paths.md), [10 — Transform](10-transform-system.md),
  [15 — Color management](15-color-management.md), [20 — File I/O](20-file-io.md),
  [24 — UI/workspace](24-ui-workspace.md).
- ADRs: [0006 — headless core](../adr/0006-headless-core-separation.md).
