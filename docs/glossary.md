# Glossary

Shared vocabulary for PhotoEdit. When a term has a precise meaning in this
project, it is defined here so the specs can use it without re-explaining.

| Term | Definition |
| --- | --- |
| **Document** | The complete editable unit: canvas, color mode, bit depth, color profile, layer tree, channels, paths, guides, metadata, and history. Not "an image." |
| **Canvas** | The document's pixel bounds (width × height at a resolution). Layers may extend beyond it. |
| **Layer** | One element of the document's stack: pixel, text, shape, adjustment, fill, smart object, or group. Has opacity, blend mode, mask, transform, visibility. |
| **Layer stack / tree** | The ordered, possibly nested set of layers the compositor walks bottom-to-top to produce the visible image. |
| **Compositor** | The engine subsystem that computes the final image (or a tile of it) by blending the layer tree. The "soul" of the app. |
| **Blend mode** | A per-pixel math function (Normal, Multiply, Screen, …) describing how a layer mixes with what's below it. |
| **Tile** | A fixed-size square block of pixels (256×256). The unit of dirty tracking, GPU upload, undo deltas, scratch paging, and threaded work. |
| **Dirty region / dirty tile** | The area changed by an edit. Only dirty tiles are recomposited/redrawn. |
| **Mask** | A grayscale buffer controlling where a layer (or filter, or selection) is visible/active. White = full, black = none, gray = partial. |
| **Selection** | A document-wide mask defining where edits are allowed. Visualized as "marching ants"; stored as an alpha buffer. |
| **Channel** | A single component plane (R, G, B, Alpha, spot, or a saved selection). |
| **Adjustment layer** | A non-destructive layer that applies a color/tonal operation (Curves, Levels, Hue/Sat…) to everything beneath it at composite time. |
| **Smart object** | A layer that embeds/links source content and renders it through a stored transform + smart filters, so the source is never destructively resampled. |
| **Smart filter** | A filter applied non-destructively to a smart object; re-editable, reorderable, removable. |
| **Non-destructive** | An edit that preserves original pixels and is computed at render time from stored parameters; reversible without data loss. |
| **Destructive** | An edit that permanently rewrites pixel data. |
| **Brush dab / stamp** | A single placement of the brush tip. A stroke is many dabs spaced along the input path. |
| **Working space** | The color space in which compositing math is performed (e.g. linear or gamma-encoded RGB at a chosen profile). |
| **ICC profile** | A standardized description of how a device/space reproduces color, used to convert between spaces. |
| **Soft proofing** | Simulating on screen how a document will look in another space/device (e.g. a CMYK printer). |
| **Bit depth** | Bits per channel: 8, 16, or 32-bit float. Higher depth avoids banding and supports HDR. |
| **Premultiplied alpha** | Color components pre-scaled by alpha; the correct space for compositing many layers. |
| **Command** | A reversible unit of mutation (`execute`/`undo`). The basis of history, scripting, actions, and batch. |
| **History** | The ordered record of commands/states enabling undo/redo and snapshots. |
| **Scratch disk** | On-disk temporary storage for tiles that don't fit in the RAM budget. |
| **RHI** | Render Hardware Interface: the engine's thin abstraction over D3D12/Vulkan for GPU work. |
| **`pe_core`** | The headless C++ engine library. No UI/Qt. |
| **App shell** | The Qt6 application (`src/app`) hosting the UI around the engine. |
| **Golden image** | A committed reference render compared against current output (within tolerance) to catch regressions. |
| **Artboard** | A named sub-canvas within a document, each with its own bounds, for multi-screen/asset design. |
| **PSD / PSB** | Photoshop's layered formats; PSB is the large-document variant. Targeted for interchange, not exact parity. |
