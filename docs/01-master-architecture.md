# 01 — Master Architecture

This is the whole system on one page: the layers of the program, how an edit
flows from input to pixels on screen, and how every subsystem plugs into a small
number of stable contracts. Each subsystem has its own spec under
[`systems/`](README.md#system-specifications); this document is the map that ties
them together.

## Layered overview

```
┌──────────────────────────────────────────────────────────────────────┐
│  App shell (src/app, Qt6)                                             │
│  menus · dockable panels · canvas viewport widget · tablet input ·     │
│  tool options · dialogs · workspace/layout · keyboard shortcuts        │
└───────────────▲───────────────────────────────────┬───────────────────┘
                │ observes (signals/notifications)   │ issues commands, reads state
                │                                     ▼
┌──────────────────────────────────────────────────────────────────────┐
│  Engine core (src/core, pe_core — NO Qt)                              │
│                                                                        │
│  ┌────────────┐   ┌──────────────┐   ┌───────────────┐                 │
│  │  Document   │   │   Commands /  │   │   Tools        │ (logic only;  │
│  │  model      │◄──│   History     │◄──│   input → cmds │  UI in shell) │
│  │ layers·     │   │  (undo/redo)  │   └───────────────┘                 │
│  │ masks·      │   └──────────────┘                                     │
│  │ channels·   │                                                        │
│  │ paths·      │   ┌──────────────────────────────────────────────┐    │
│  │ selection·  │──▶│  Compositor (layer tree → tiles)             │    │
│  │ metadata    │   │  blend modes · adjustments · masks · effects  │    │
│  └─────┬───────┘   └───────────────▲──────────────────────────────┘    │
│        │                           │ pulls tiles                        │
│        │  ┌────────────┐  ┌────────┴────────┐  ┌───────────────────┐    │
│        └─▶│  Pixel/tile │  │  Color engine   │  │  Filters / brush  │    │
│           │  storage     │  │  (ICC, depth)   │  │  generators       │    │
│           └─────┬───────┘  └─────────────────┘  └───────────────────┘    │
│                 │                                                        │
│   ┌─────────────┴───────────────┐   ┌────────────────────────────────┐  │
│   │  Performance layer           │   │  RHI (GPU abstraction)         │  │
│   │  RAM budget · tile cache ·   │   │  D3D12 / Vulkan / software     │  │
│   │  scratch disk · thread pool  │   └────────────────────────────────┘  │
│   └─────────────────────────────┘                                       │
│                                                                        │
│   ┌──────────────────────────────────────────────────────────────────┐ │
│   │  File I/O (native doc, PSD, PNG/JPEG/TIFF/WebP, raw)              │ │
│   └──────────────────────────────────────────────────────────────────┘ │
│   ┌──────────────────────────────────────────────────────────────────┐ │
│   │  Automation / scripting · Plugin host · AI provider              │ │
│   └──────────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────┘
```

The hard rule that makes this tractable: **everything below the top box is
headless C++ with no UI dependency.** The shell is a *client* of the engine.

## The core contracts

Almost every feature plugs into one of a handful of stable interfaces. Get these
right and the 29 systems compose cleanly.

### 1. `Document` — the single source of truth

Owns the entire editable state. All mutation goes through commands; the rest of
the system observes it.

```cpp
class Document {
public:
    Size canvasSize() const;
    int   resolutionPpi() const;
    ColorMode colorMode() const;     // RGB, CMYK, Gray, Lab, Indexed, Bitmap
    BitDepth  bitDepth() const;      // 8, 16, 32f
    const ColorProfile& profile() const;

    const LayerTree&  layers() const;
    const Selection&  selection() const;
    const ChannelSet& channels() const;
    const PathSet&    paths() const;
    const Metadata&   metadata() const;

    History&  history();             // execute/undo commands
    bool isDirty() const;            // unsaved changes
};
```

See [systems/01-document-system.md](systems/01-document-system.md).

### 2. `Command` — one reversible edit

The universal unit of change. Undo/redo, scripting, actions, and batch are all
just *sequences of commands*. This is the second pillar (with the Document) that
the whole architecture rests on.

```cpp
class Command {
public:
    virtual ~Command() = default;
    virtual std::string name() const = 0;     // for the History panel
    virtual void execute(Document&) = 0;
    virtual void undo(Document&) = 0;
    virtual bool serialize(Writer&) const = 0; // for actions/scripts/recording
    // Memory hint so History can decide what to keep in RAM vs. drop/recompute.
    virtual size_t approximateCost() const { return 0; }
};
```

See [systems/21-history-undo.md](systems/21-history-undo.md).

### 3. `Layer` — a node in the tree

A polymorphic node. Pixel, text, shape, adjustment, fill, smart-object, and group
layers all satisfy the same contract so the compositor treats them uniformly.

```cpp
class Layer {
public:
    LayerKind kind() const;
    std::string name() const;
    bool   visible() const;
    float  opacity() const;          // [0,1]
    BlendMode blendMode() const;
    const Mask* mask() const;        // optional layer mask
    const Transform& transform() const;

    // Render this layer's contribution for the given tile region, into the
    // working color space / bit depth, honoring its own effects but NOT its
    // blend/opacity (the compositor applies those).
    virtual void renderInto(TileRequest, TileSink&) const = 0;
};
```

See [systems/03-layer-system.md](systems/03-layer-system.md).

### 4. The compositor — tree → tiles

Given a region (a set of tiles) and the layer tree, produce the composited
result. This loop is the soul of the program:

```
compositeTile(region):
    result = transparent
    for layer in tree bottom→top:
        if not layer.visible: continue
        src = layer.renderInto(region)        # pixels in working space
        src = applyLayerEffects(src, layer)   # styles/effects
        src = applyMask(src, layer.mask, region)
        result = blend(result, src, layer.blendMode, layer.opacity)
    return result            # for adjustment layers, 'src' transforms 'result'
```

Adjustment layers are a twist on the loop: instead of contributing pixels, they
*transform the accumulated `result`* beneath them. Groups recurse. The compositor
operates per tile so only dirty tiles are recomputed.

See [systems/02-canvas-rendering.md](systems/02-canvas-rendering.md) and
[systems/04-blend-modes.md](systems/04-blend-modes.md).

### 5. `Tool` — input becomes commands

Tools live in the engine as logic (state machines that turn pointer/keyboard
events into commands and overlays); their *UI* (cursors, option bars) lives in the
shell.

```cpp
class Tool {
public:
    virtual void onPointerDown(const PointerEvent&, ToolContext&) = 0;
    virtual void onPointerMove(const PointerEvent&, ToolContext&) = 0;
    virtual void onPointerUp(const PointerEvent&, ToolContext&) = 0;
    virtual void drawOverlay(OverlaySink&) const {}   // marching ants, handles
    // On commit, the tool pushes a Command onto the document's history.
};
```

See [systems/09-tool-system.md](systems/09-tool-system.md).

### 6. RHI — the GPU contract

A thin render-hardware interface the engine uses for display compositing and
data-parallel work, with backends for D3D12 (Windows first), Vulkan, and a
software fallback. The CPU reference path defines correctness; GPU must match.

See [systems/23-gpu-acceleration.md](systems/23-gpu-acceleration.md) and
[ADR-0002](adr/0002-gpu-abstraction.md).

## Data flow: the life of a brush stroke

This is the canonical end-to-end path; most edits are variations on it.

```
1. User drags with the Brush tool (tablet pressure/tilt) over the canvas widget.
2. App shell translates Qt events → engine PointerEvents → active Tool.
3. BrushTool stabilizes the input, generates dabs along the path, and builds a
   PaintCommand targeting the active layer, respecting the active selection,
   the layer mask, brush opacity/flow/blend mode, color, bit depth, and locks.
4. PaintCommand.execute(): rasterizes dabs into the layer's tiles. It records
   the prior contents of touched tiles (for undo) and marks them dirty.
5. The dirty region is unioned and handed to the compositor.
6. Compositor recomposites ONLY the dirty tiles through the layer tree, in the
   working color space at the document's bit depth.
7. Composited tiles are converted through the display profile and uploaded to
   the GPU via the RHI; the canvas viewport repaints just those tiles.
8. History gains one undoable command; the document becomes dirty; panels
   (Layers, Histogram, Navigator) observe and refresh.
```

Every numbered step is a subsystem boundary. The same skeleton — *tool → command
→ tile mutation → dirty region → recomposite → color-convert → display* — applies
to fills, filters, transforms, retouching, and generative edits.

## Threading model

- **Document mutation** happens on a single logical owner (the "document thread")
  so the model needs no fine-grained locking. Commands execute there.
- **Heavy, data-parallel work** — compositing many tiles, filters, exports,
  thumbnails, raw decode — is dispatched to a **worker thread pool**, partitioned
  by tile so workers never alias the same pixels.
- **The GPU** is driven from a render thread that consumes immutable composited
  tiles.
- **I/O** (loading, saving, scratch paging) runs off the document thread so the UI
  never blocks; results are merged back via tasks.

The performance layer owns the thread pool, the RAM budget, the tile cache, and
scratch-disk paging. See [systems/22-performance.md](systems/22-performance.md).

## Memory & large documents

The engine assumes documents can exceed RAM:

- Layer pixels live in **tiles**; tiles are reference-counted and shared
  copy-on-write where possible (e.g. undo snapshots share unchanged tiles).
- A **RAM budget** bounds resident tiles; cold tiles spill to the **scratch
  disk** and are faulted back on demand.
- The compositor and filters are **lazy and dirty-driven**: nothing recomputes
  unless its inputs changed.
- Previews can render at reduced resolution while full-res computes in the
  background.

## Color pipeline (where it sits)

Compositing happens in a defined **working space** at the document's bit depth.
Inputs (placed images, opened files) are converted *into* that space on import;
output to the screen goes through the **display profile**, and export/print
through the chosen **output profile**. Color is never treated as bare RGB.

See [systems/15-color-management.md](systems/15-color-management.md).

## Persistence

- The **native document format** round-trips the entire model (layers, masks,
  channels, paths, smart objects, history-optional, metadata, color profile).
- **PSD/PSB** is supported for interchange (faithful on common structure).
- Flat raster formats (PNG/JPEG/TIFF/WebP/…) export a flattened/managed result.

See [systems/20-file-io.md](systems/20-file-io.md).

## Extensibility (designed in, delivered late)

Because every edit is a `Command` and the engine is headless:

- **Automation** (actions/batch/scripts) records and replays commands.
- **Plugins** register tools, filters, file formats, and panels against the same
  contracts the built-ins use.
- The **AI system** is a provider behind an interface: selection+prompt+context
  in, layers out — local or cloud.

See [automation](systems/26-automation.md), [plugins](systems/29-plugin-extension.md),
[AI](systems/14-generative-ai.md).

## Why this architecture holds up

The difficulty in a Photoshop-class app is not any single feature; it is that
features interact. This design contains that complexity in three ideas:

1. **One source of truth** (the Document) mutated through **one kind of operation**
   (the Command) — so undo, scripting, actions, and collaboration are one
   mechanism, not four.
2. **One rendering model** (the tile compositor over a uniform Layer contract) —
   so every layer type, blend mode, mask, adjustment, and filter slots into the
   same loop.
3. **One performance substrate** (tiles + dirty regions + budgeted cache +
   scratch + thread pool) — so "works on a 30k×30k document" is a property of the
   foundation, not a per-feature fight.

Everything in [`systems/`](README.md#system-specifications) is an elaboration of
these three ideas.
