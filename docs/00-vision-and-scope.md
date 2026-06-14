# 00 — Vision & Scope

## The one-sentence vision

PhotoEdit is a professional, GPU-accelerated, color-managed 2D image editor —
a "Photoshop-class" application — built natively for Windows first, with a
headless engine that can grow onto other platforms later.

## What "flagship level" means here

This is not a paint app with layers bolted on. The defining property of a
professional editor is that **every feature is non-destructive and composable**,
and that the program stays fast and correct on documents far larger than RAM.
Concretely, flagship level means:

- **A real document model**, not "a bitmap." Canvas size, resolution, color
  mode, bit depth, an ICC color profile, a tree of layers/groups/masks,
  channels, paths, guides, metadata, and a full history.
- **Non-destructive everything**: adjustment layers, smart objects, smart
  filters, layer/vector masks, and generative layers that never overwrite source
  pixels.
- **A tiled engine** that only recomputes the dirty region after each edit and
  pages cold tiles to a scratch disk, so a 30,000 × 30,000 px document is usable.
- **Color correctness**: compositing in a defined working space at 8/16/32-bit,
  ICC-based display and print conversion, soft proofing, gamut warnings.
- **Pro I/O**: a layered native format plus PSD interchange, and the common
  raster formats, all preserving as much document structure as the format allows.
- **Automation & extensibility**: actions, batch, scripting, and a stable plugin
  API — which fall out naturally from a command-based architecture.

## Target users

1. **Photographers** — raw workflow, retouching, color grading, print.
2. **Digital painters / illustrators** — brush engine, tablet pressure, masks.
3. **Designers / compositors** — layers, smart objects, text, vector shapes.
4. **Production pipelines** — automation, batch export, scripting, plugins.

We optimize for the professional who would otherwise use Photoshop or Krita and
expects that level of capability, performance, and color fidelity.

## Principles (every design decision answers to these)

1. **Non-destructive by default.** Destroying original pixels is a last resort,
   always explicit. The compositor computes the visible image from a recipe.
2. **The engine is headless.** All image logic lives in `pe_core` with no UI
   dependency, so it is testable, scriptable, and portable. The UI is a client
   of the engine, never the other way around.
3. **Correctness is defined by a reference.** Every blend/filter/transform has a
   simple, obviously-correct CPU reference implementation. Fast paths (SIMD/GPU)
   must match it within tolerance, enforced by tests.
4. **Big documents are the normal case.** Tiles, dirty regions, lazy
   compositing, and scratch-disk paging are designed in from M1, not retrofitted.
5. **One edit = one command.** Every mutation is a reversible command. This is
   what makes undo, history, scripting, actions, and batch all the same machine.
6. **Color is never "just RGB."** Pixels carry a known color space and bit
   depth; conversions are explicit and managed.
7. **Ship in steps.** Each milestone is independently buildable and demoable. We
   never have "everything half-built and broken."
8. **Windows first, portable underneath.** We target Windows/MSVC as the primary
   platform but make no decision that forecloses macOS/Linux. The hard parts
   (engine, color, GPU abstraction) are cross-platform from day one.

## In scope (the full system, delivered over M0–M10)

The complete feature surface is enumerated across the
[system specifications](README.md#system-specifications): document, layers,
blend modes, adjustments, masks, selections, brushes, tools, transforms, smart
objects, filters, retouching, generative AI, color management, camera raw, text,
vector/paths, channels, file I/O, history, performance, GPU, UI/workspace,
presets, automation, print/prepress, cloud, and plugins.

## Out of scope (at least initially)

- **Video and audio editing**, timeline animation, and 3D (OBJ/DAE) workflows.
  The architecture must not preclude them, but they are not targeted.
- **Mobile / tablet OS apps.** Desktop only.
- **Real-time multi-user co-editing.** Cloud sync of documents may come; live
  collaboration is not a near-term goal.
- **Bit-for-bit PSD round-trip fidelity** for every exotic Photoshop feature. We
  target faithful interchange of the common, important structure (layers, masks,
  groups, basic adjustments, text where feasible), not 100% parity with every
  legacy PSD record.
- **Reimplementing Adobe's proprietary AI models.** The generative system is
  designed around a pluggable model provider (local or cloud); we supply the
  pipeline and integration, not a from-scratch foundation model.

## Definition of done for the product

A user can: open or create a color-managed document; build a non-destructive
composition of pixel, text, shape, adjustment, and smart-object layers with
masks; paint with a pressure-sensitive brush; select and retouch; apply
filters and generative edits; and export to the right format for screen or
print — on a document larger than memory, without the app becoming unusable.

We are deliberately a long way from that today. M0 lays the foundation that
makes the rest tractable. See the [roadmap](03-roadmap-and-milestones.md).
