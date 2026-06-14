# 27 — Printing / Prepress

> Milestone: M10 · Status: Spec

## Purpose

PhotoEdit targets print, not just screens. Printing/prepress covers CMYK
conversion, ICC printer profiles, soft proofing, gamut warnings, spot colors,
DPI/PPI and print scaling, bleed/crop, and PDF/TIFF export for press. This is the
"boring but professional" layer that makes output reproduce correctly, and it
builds directly on [color management](15-color-management.md) (lcms2) and
[channels](19-channels.md): a monitor *emits* light while paper *reflects* it, so
the same RGB will not reproduce in print without a profile conversion.

## Requirements

**Functional**

- Convert documents to CMYK (or keep RGB) using an output/printer ICC profile with
  a chosen rendering intent and black-point compensation.
- **Soft proofing**: simulate the print result on screen, including paper white and
  ink black simulation.
- **Gamut warning**: flag colors outside the destination gamut.
- **Spot colors**: named ink [channels](19-channels.md) preserved through export.
- Print scaling, resolution (PPI), bleed and crop marks.
- Export press-ready **PDF** and **TIFF** (with profiles embedded/converted).

**Non-functional**

- `pe_core` performs all color/spot/flatten work headlessly; the print dialog UI
  lives in the [app shell](24-ui-workspace.md).
- Conversions are color-accurate and reproducible.

## Data model

```cpp
namespace pe {

struct SoftProofSettings {
    ColorProfile destination;     // e.g. a CMYK press profile
    RenderingIntent intent = RenderingIntent::RelativeColorimetric;
    bool blackPointCompensation = true;
    bool simulatePaperWhite = false;
    bool simulateBlackInk = false;
    bool gamutWarning = false;
};

struct PrintSettings {
    ColorProfile printerProfile;
    RenderingIntent intent;
    bool blackPointCompensation = true;
    float scalePercent = 100.0f;
    int  resolutionPpi = 300;
    Margins bleed;                // bleed area
    bool cropMarks = false;
};

} // namespace pe
```

## Behavior & algorithms

**Print/proof pipeline:**

```
working-space composite
  → (optional) flatten with profile-aware blending
  → convert working → destination profile (lcms2 transform, intent + BPC)
  → for soft proof: convert destination → display profile and show on screen,
    optionally simulating paper white / black ink
  → for gamut warning: mark pixels whose working color is out of destination gamut
  → for print/export: emit converted data (+ spot channels) to PDF/TIFF
```

Soft proofing reuses the [color management](15-color-management.md) transform
cache keyed by (source, destination, intent, BPC). Spot colors are carried as
named channels and written into the export rather than being converted to process
colors. PDF export embeds the destination profile (or converts) and includes
crop/bleed geometry.

## Interactions

- [Color management](15-color-management.md): the engine underneath (ICC, intents,
  BPC, lcms2 transforms) — this system is its print-facing application.
- [Channels](19-channels.md): spot channels for named inks.
- [File I/O](20-file-io.md): PDF/TIFF prepress export.
- [Document system](01-document-system.md): color mode (RGB↔CMYK) and resolution.

## Performance, threading & GPU

- Conversions are tile-parallel on the [worker pool](22-performance.md); soft-proof
  display conversion can run on the [GPU](23-gpu-acceleration.md).
- Gamut-check is a per-pixel test cached per tile.

## Edge cases & failure modes

- Missing printer profile → block conversion with a clear message; offer a default.
- Rich-black / total-ink-limit violations → optional ink-limit check and warning.
- Spot color without a defined ink → placeholder + warning.
- RGB-only formats receiving a CMYK document → convert or refuse per export rules.

## Testing strategy

- Unit: a known RGB patch through a known profile/intent yields expected CMYK (vs
  lcms2 reference values).
- Gamut-warning flags a deliberately out-of-gamut patch and not an in-gamut one.
- Spot channels survive a PDF/TIFF export→reimport round-trip.

## Phasing

- **M6 (groundwork)**: soft proofing, gamut warning, rendering intents land with
  color management.
- **M10**: CMYK conversion workflows, spot colors, print scaling/bleed/crop, PDF/
  TIFF press export, ink-limit checks.

## Open questions

- PDF generation library/approach for press-grade output.
- Total-ink-limit and overprint handling depth.
- Whether to ship a CMYK editing mode or proof-only initially.

## References (relative links)

- [Glossary](../glossary.md) — Soft proofing, ICC profile, Channel.
- Sibling systems: [15 — Color management](15-color-management.md),
  [19 — Channels](19-channels.md), [20 — File I/O](20-file-io.md),
  [01 — Document system](01-document-system.md).
- ADRs: [0004 — color management](../adr/0004-color-management.md).
