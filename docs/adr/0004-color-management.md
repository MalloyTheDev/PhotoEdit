# ADR-0004 — Color-managed pipeline on Little-CMS 2

**Status:** Accepted

## Context

Professional output requires that color be reproduced correctly across monitors,
print, cameras, and export. The same RGB numbers mean different colors in
different spaces. A flagship editor must support ICC profiles, multiple color
modes (RGB/CMYK/Gray/Lab), 8/16/32-bit depth, soft proofing, gamut warnings, and
correct compositing math. Writing a correct CMM (color management module) from
scratch is a multi-year effort and a correctness minefield.

## Decision

Adopt **Little-CMS 2 (lcms2)** as the color-management engine, and make color
explicit throughout:

- Every document carries a **color mode**, **bit depth**, and **ICC profile**.
- Compositing happens in a defined **working space** at the document's bit depth;
  blend/filter math is done in float.
- Inputs are converted **into** the working space on import; the screen is fed via
  the **display profile**; export/print via the chosen **output profile**.
- Soft proofing, rendering intents, black-point compensation, and gamut warnings
  are implemented via lcms2 transforms.

## Consequences

**Positive**
- A battle-tested ICC engine (used widely, including by Krita) instead of a
  bespoke CMM; correctness and broad profile support out of the box.
- Color correctness is designed in from M1 (working-space compositing) and fully
  realized in M6, not bolted on later.

**Negative / costs**
- An extra native dependency and the discipline of always tracking pixel color
  space + depth.
- 16/32-bit paths increase memory and compute; mitigated by tiles and the budget.

## Alternatives considered

- **Bare sRGB, "just RGB" everywhere.** Simple but disqualifying for professional
  print/photo work; rejected outright per the vision.
- **Write our own CMM.** Full control, but enormous effort and risk for no real
  benefit over lcms2. Rejected.
- **OpenColorIO.** Excellent for film/VFX pipelines (config-driven, log/scene-
  linear), but oriented to that world; ICC-centric photo/print workflows are
  better served by lcms2. We may add OCIO support later for VFX interop.

## Notes

Details and the staged 8→16→32-bit rollout are in
[systems/15-color-management.md](../systems/15-color-management.md) and
[systems/19-channels.md](../systems/19-channels.md).
