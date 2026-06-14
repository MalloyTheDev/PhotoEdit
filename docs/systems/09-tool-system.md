# 09 — Tool System

> Milestone: M3 · Status: Spec

## Purpose

The UI of a Photoshop-class editor is built *around tools* — the user is almost
always "in" a tool, and the cursor, the options bar, and what a drag does are all
the active tool's behavior. This document specifies the engine **`Tool`
framework**: the contract every tool satisfies, the shared `ToolContext` it
operates on, how tools are registered and activated, the tool-options model the
[shell](24-ui-workspace.md) renders into the options bar, and the discipline that
every tool **commits exactly one [`Command`](21-history-undo.md)** (or one
coalesced command for a drag).

The hard line from the [architecture](../01-master-architecture.md#5-tool--input-becomes-commands)
holds: **tools are engine logic** — state machines that turn pointer/keyboard
events into commands and overlays — while their **UI lives in the shell**
(cursors, the options bar, contextual task bars). The engine `Tool` knows
nothing about Qt; it receives already-translated `PointerEvent`s and emits
commands and overlay draw-lists. This split is what lets the same tools be
driven by [automation](26-automation.md) and tests without a window.

Many tools are thin front-ends over a heavyweight engine documented elsewhere:
the Brush tool drives the [brush engine](08-brush-engine.md), the selection tools
drive the [selection system](07-selection-system.md), Transform drives the
[transform system](10-transform-system.md), the retouching tools drive
[retouching](13-retouching.md), and the Pen/Shape tools drive
[vector & paths](18-vector-paths.md). This system owns the *framework* that makes
them all uniform; each tool's deep behavior is its own spec.

## Requirements (Functional + Non-functional)

**Functional**

- Define a `Tool` interface (on the `Tool` contract from the master architecture):
  `onPointerDown/Move/Up`, `onKeyDown/Up`, `onWheel`, `drawOverlay`, `activate`,
  `deactivate`, plus identity/metadata for the toolbox.
- Provide a `ToolContext` giving every tool uniform access to the active
  [document](01-document-system.md), active layer, active
  [selection](07-selection-system.md), foreground/background color, the **viewport
  transform** (document↔screen mapping for hit-testing and overlays), modifier-key
  state, the [history](21-history-undo.md), and the pressure/tilt of the current
  pointer.
- A **ToolManager** owning the registry, the active tool, activation/deactivation,
  default tool, and temporary "spring-loaded" tool switching (hold a key to use a
  tool, release to revert).
- A **tool-options model**: each tool exposes a typed, observable set of options
  (e.g. brush size, feather, tolerance, mode flags) that the shell renders into
  the options bar and persists as **tool presets** ([presets](25-presets-assets.md)).
- A rule that a tool produces **exactly one Command per interaction**: a click or
  a full drag becomes one undoable step (drag deltas are *coalesced* into one
  command), so undo steps match user intent, not raw events.
- Support **overlays**: marching ants, transform handles, crop shades, path nodes,
  measurement readouts — drawn by the tool into an `OverlaySink`, composited above
  the image by the shell (never baked into pixels).
- Cover all tool **categories** and canonical tools (enumerated below).
- Let tools declare their **cursor** and **status hint** abstractly (the shell
  maps these to real Qt cursors / status-bar text).

**Non-functional**

- `pe_core`, pure C++20, **no Qt** ([ADR-0006](../adr/0006-headless-core-separation.md)).
  The shell translates Qt mouse/tablet/key events into engine `PointerEvent`/
  `KeyEvent` and renders option widgets from the model; tools never see widgets.
- Tools run on the **document thread** with the model; heavy work they trigger
  (a filter, a big fill) is dispatched to workers by the command, not done inline
  in an event handler.
- Tool switching is instant and never loses an in-flight edit (activating a new
  tool first commits or cancels the current tool's interaction).
- A tool's per-event handlers are cheap; interactive feedback is overlays +
  incremental previews, with the authoritative result on commit.

## Data model (concrete C++ in `namespace pe`)

Building on `Point`/`Rect` (`pe/core/Geometry.hpp`), `Rgba8`/`Rgbaf`
(`pe/core/Color.hpp`), the `Command` contract, and `Vec2`/`StrokePoint` from the
[brush engine](08-brush-engine.md).

```cpp
namespace pe {

// Pointer device kind, so tools can adapt (e.g. tablet vs mouse smoothing).
enum class PointerDevice : uint8_t { Mouse, Pen, Eraser, Touch };

enum class PointerPhase : uint8_t { Down, Move, Up, Hover, Cancel };

// Modifier keys, as flags (shell maps Qt::KeyboardModifiers to these).
enum class Modifiers : uint32_t {
    None = 0, Shift = 1<<0, Ctrl = 1<<1, Alt = 1<<2, Meta = 1<<3, Space = 1<<4
};

// A translated input event in DOCUMENT space (sub-pixel). The shell has already
// applied the inverse viewport transform; tools think in document coordinates.
struct PointerEvent {
    PointerPhase  phase = PointerPhase::Move;
    PointerDevice device = PointerDevice::Mouse;
    Vec2   pos;                 // document-space, sub-pixel
    Vec2   screenPos;           // raw screen px (for hit-testing handles at fixed size)
    float  pressure = 1.0f;
    float  tiltX = 0.0f, tiltY = 0.0f, rotation = 0.0f;
    Modifiers mods = Modifiers::None;
    int    button = 0;          // 0=left,1=right,2=middle
    double timeMs = 0.0;
};

struct KeyEvent { int key = 0; Modifiers mods = Modifiers::None; bool repeat = false; };

// Document<->screen mapping the tool needs for overlays and screen-space hit
// tests (e.g. a handle that stays 8 screen-px regardless of zoom).
struct ViewportTransform {
    Vec2  pan{0,0};
    float zoom = 1.0f;          // screen px per document px
    float rotationRad = 0.0f;   // rotate-view
    [[nodiscard]] Vec2 docToScreen(Vec2) const noexcept;
    [[nodiscard]] Vec2 screenToDoc(Vec2) const noexcept;
};

// Everything a tool may read/affect, handed in on every event.
class ToolContext {
public:
    Document&  document() noexcept;
    History&   history() noexcept;                 // commit commands here
    [[nodiscard]] LayerId activeLayer() const noexcept;
    [[nodiscard]] const Selection& selection() const noexcept;
    [[nodiscard]] Rgba8 foreground() const noexcept;
    [[nodiscard]] Rgba8 background() const noexcept;
    [[nodiscard]] const ViewportTransform& viewport() const noexcept;
    [[nodiscard]] Modifiers modifiers() const noexcept; // live modifier state

    // Tools request a viewport change (pan/zoom/rotate) without mutating the doc:
    void requestViewport(const ViewportTransform&);
    // Abstract cursor + status hint; the shell maps these to Qt.
    void setCursor(CursorHint);
    void setStatus(std::string_view);
};

// What a tool draws above the image (never written to pixels). The shell renders
// these with the GPU/QPainter overlay layer.
class OverlaySink {
public:
    void line(Vec2 a, Vec2 b, OverlayStyle);
    void rect(Rect, OverlayStyle);
    void marchingAnts(const PathOutline&);          // animated selection edge
    void handle(Vec2 screenPos, HandleKind);        // transform/crop/path handle
    void text(Vec2 screenPos, std::string_view);    // measurement readout
};

// One typed tool option (the shell builds an options-bar widget from this).
struct ToolOption {
    enum class Type : uint8_t { Float, Int, Bool, Enum, Color, BlendMode, BrushPreset };
    std::string id;             // stable key (also used by tool presets/scripts)
    std::string label;
    Type   type = Type::Float;
    double value = 0.0;         // numeric/bool; enum=index
    double min = 0.0, max = 1.0, step = 0.01;
    std::vector<std::string> enumLabels;            // for Enum
};

class ToolOptions {
public:
    [[nodiscard]] std::span<ToolOption> all() noexcept;
    [[nodiscard]] ToolOption* find(std::string_view id) noexcept;
    void set(std::string_view id, double v);        // notifies observers (options bar)
    void addObserver(ToolOptionsObserver*);
};

// Toolbox metadata for grouping/shortcuts (UI consumes this).
struct ToolInfo {
    std::string id;             // "brush", "marquee.rect", ...
    std::string name;
    ToolCategory category;      // see enum below
    char        shortcut = 0;   // primary key; cycling handled by ToolManager
    CursorHint  defaultCursor;
};

class Tool {
public:
    virtual ~Tool() = default;
    [[nodiscard]] virtual const ToolInfo& info() const = 0;
    virtual ToolOptions& options() = 0;

    virtual void activate(ToolContext&) {}
    virtual void deactivate(ToolContext&) {}        // must commit/cancel in-flight work

    virtual void onPointerDown(const PointerEvent&, ToolContext&) = 0;
    virtual void onPointerMove(const PointerEvent&, ToolContext&) = 0;
    virtual void onPointerUp(const PointerEvent&, ToolContext&) = 0;
    virtual void onKeyDown(const KeyEvent&, ToolContext&) {}
    virtual void onKeyUp(const KeyEvent&, ToolContext&) {}
    virtual void onWheel(float deltaY, const PointerEvent&, ToolContext&) {}

    virtual void drawOverlay(OverlaySink&, const ToolContext&) const {}
    [[nodiscard]] virtual CursorHint cursorFor(const PointerEvent&, const ToolContext&) const;
};

enum class ToolCategory : uint8_t {
    Navigation, Selection, Crop, Retouching, Painting, ToneDodgeBurn,
    Vector, Type, Sampler, Transform, Measure, Ai
};

class ToolManager {
public:
    void registerTool(std::unique_ptr<Tool>);       // built-ins + plugins (29)
    [[nodiscard]] Tool* active() const noexcept;
    void activate(std::string_view id, ToolContext&);// deactivates current first
    void pushTemporary(std::string_view id, ToolContext&); // spring-loaded (hold key)
    void popTemporary(ToolContext&);
    [[nodiscard]] std::span<const ToolInfo> toolbox() const noexcept;
};

} // namespace pe
```

## Behavior & algorithms

### Event flow and the one-command rule

The shell owns the Qt event loop. On each mouse/tablet/key event it:

```
shell event handler:
    pe = translate(qtEvent)                     # -> PointerEvent in document space
    tool = toolManager.active()
    switch pe.phase:
        Down: tool.onPointerDown(pe, ctx)       # tool begins an interaction
        Move: tool.onPointerMove(pe, ctx)       # accumulate; update overlay/preview
        Up:   tool.onPointerUp(pe, ctx)         # tool commits exactly one Command
    requestRepaintOverlays()                    # drawOverlay re-runs as needed
```

The **one-command rule** is the discipline that makes history match intent. A
drag fires many `Move` events, but the tool buffers them and pushes a **single**
command on `Up` (or coalesces incremental commands — see below). So a brush
stroke, a marquee drag, a free-transform manipulation, or a gradient drag each
produce exactly one undo step. Two implementation patterns:

1. **Build-then-commit** (most tools): the tool accumulates interaction state
   (e.g. the [brush](08-brush-engine.md) stroke buffer, the marquee rect, the
   transform matrix) and constructs one `Command` at `Up`. Interactive feedback
   is an overlay and/or an incremental preview, not committed pixels.
2. **Coalesced live command**: for tools that must mutate the model live (e.g. a
   parametric Transform that the user nudges with multiple drags before pressing
   Enter), the tool executes a mutable command and *merges* subsequent
   adjustments into it (`Command::mergeWith`) so the whole manipulation collapses
   into one history entry on confirm. The [history](21-history-undo.md) system
   defines the coalescing/merge window.

`deactivate` (on tool switch) must **finish the interaction**: commit if there is
a valid in-flight edit, otherwise cancel cleanly. Switching tools never strands a
half-applied edit or leaks a preview overlay.

### Registration, activation, spring-loaded tools

Built-in tools register at startup; [plugins](29-plugin-extension.md) register
the same way against the identical contract. `ToolManager::activate` deactivates
the current tool first, then activates the new one (giving it the context to set
its cursor and options). **Spring-loaded** switching (`pushTemporary`/
`popTemporary`) implements "hold `H` to pan, release to return to Brush": the
manager stashes the current tool, runs the temporary one for the key's duration,
and restores on release — committing/cancelling each correctly.

### Tool options and the options bar

A tool's `ToolOptions` is the contract between engine behavior and the shell's
options bar. The shell reads `options().all()`, builds the appropriate widgets
from each `ToolOption.type`/range, and on user change calls `set(id, value)`,
which notifies the tool (and any observers) — keeping engine state and UI in
sync without the engine knowing about widgets. Tool presets
([presets](25-presets-assets.md)) are just serialized `ToolOptions` snapshots.

### Tool categories and the canonical set

Every tool slots into a `ToolCategory`; the deep behavior lives in the linked
system, the *framework wiring* lives here.

- **Navigation** — **Hand** (pan via `requestViewport`), **Zoom** (scroll/marquee
  zoom), **Rotate View** (non-destructive canvas rotation). These mutate the
  `ViewportTransform`, **never the document**, so they push no commands.
- **Selection** — **Move** (move layer/selection content → a transform/move
  command), **Marquee** (rect/ellipse/row/column), **Lasso** family (freehand,
  polygonal, magnetic), **Object Selection**, **Magic Wand**, **Quick Selection**.
  All drive the [selection system](07-selection-system.md); modifiers do
  add/subtract/intersect.
- **Crop** — **Crop** and **Perspective Crop** (commit a canvas-size command),
  **Slice** tools (export regions).
- **Retouching** — **Clone Stamp**, **Healing Brush**, **Spot Healing**,
  **Patch**, **Remove**, **Red Eye**. These are brush-like tools that drive
  [retouching](13-retouching.md) (often sampling a source point/region first).
- **Painting** — **Brush**, **Pencil**, **Eraser**, **Mixer Brush** (drive the
  [brush engine](08-brush-engine.md)); **Gradient** (drag defines the axis →
  gradient fill command); **Paint Bucket** (flood fill by tolerance → fill
  command).
- **Tone (Dodge/Burn/Sponge)** — brush-like tools that lighten/darken/saturate;
  brush-engine front-ends with a tonal blend op.
- **Vector / drawing** — **Pen** (and Freeform/Curvature Pen), **Shape** tools
  (rect, ellipse, polygon, line, custom), **Path Selection** / **Direct
  Selection** (move paths / edit anchor points). Drive
  [vector & paths](18-vector-paths.md); commit path or shape-layer commands.
- **Type** — **Horizontal/Vertical Type**, **Type Mask**, type-on-path. Manage an
  editable [text](17-text-typography.md) layer's caret/selection; commit text
  edits as commands.
- **Sampler** — **Eyedropper** (set foreground from a pixel; samples the
  composite or active layer, honoring color management), **Color Sampler**
  (persistent readout points), **Ruler**/**Note**, **Count**.
- **Transform** — **Free Transform** and the move/scale/rotate/skew/distort/
  perspective/warp modes, on-canvas rotation/tilt — the framework for these is
  here; the math and resampling are in [transform](10-transform-system.md).
- **Measure** — **Ruler**, **Count**, **Color Sampler** readouts (overlay-only,
  no pixel mutation).
- **AI** — **Generative Fill/Expand**, object **Remove**, **Select Subject**:
  thin tools that gather a selection + prompt + context and hand off to the
  [generative AI](14-generative-ai.md) provider, which returns a layer (one
  command).

## Interactions (Document/Command/Layer/compositor/Tool/RHI + sibling links)

- **[Document](01-document-system.md) / [history](21-history-undo.md)** — tools
  read the document through `ToolContext` and mutate it **only** by pushing
  `Command`s to `history()`; the one-command rule maps each interaction to one
  undo step ([ADR-0005](../adr/0005-command-history-model.md)).
- **[Brush engine](08-brush-engine.md)** — Brush/Pencil/Eraser/Mixer/Dodge/Burn/
  Sponge feed `StrokePoint`s in and commit the `PaintCommand`.
- **[Selection system](07-selection-system.md)** — selection tools produce/modify
  the document selection; the active selection then gates other tools.
- **[Transform system](10-transform-system.md)** — Free Transform / Move use the
  framework here; the matrix + resampling and handle interaction live there.
- **[Retouching](13-retouching.md)** & **[vector & paths](18-vector-paths.md)** &
  **[text](17-text-typography.md)** & **[generative AI](14-generative-ai.md)** —
  the retouch, pen/shape, type, and AI tools are front-ends over those systems.
- **[Compositor / canvas](02-canvas-rendering.md)** — commits produce dirty
  regions the compositor consumes; overlays draw *above* the composite via the
  shell, never into pixels.
- **[RHI / GPU](23-gpu-acceleration.md)** — overlays and previews are rendered by
  the shell's overlay pass; tools only emit abstract draw-lists into
  `OverlaySink`.
- **[Presets & assets](25-presets-assets.md)** — tool presets and brush presets
  are saved `ToolOptions`/`BrushSettings`.
- **[Automation](26-automation.md)** — because tools commit serializable commands,
  recorded actions replay tool *results*; scripts can also construct the same
  commands directly without the tool.
- **[Plugins](29-plugin-extension.md)** — third-party tools register against this
  exact `Tool`/`ToolOptions` contract.
- **App shell ([UI/workspace](24-ui-workspace.md))** — owns the toolbox, options
  bar, cursors, status bar, keyboard shortcuts, and Qt event translation; renders
  what the engine tools describe abstractly.

## Performance, threading & GPU

- Tool event handlers run on the **document thread** and must stay light: they
  update interaction state and overlays, not pixels. Anything heavy (flood fill,
  filter, AI request) is encapsulated in the command, which dispatches to the
  [worker pool](22-performance.md) or runs async, so the event handler returns
  immediately.
- **Overlays are cheap and GPU-friendly**: vector draw-lists (lines, handles,
  marching ants) rendered each frame by the shell; they cost nothing in the image
  compositor and require no recomposite.
- **Incremental previews** (brush stroke, transform, gradient) reuse the
  dirty-tile path so feedback scales with the affected area, not document size.
- Spring-loaded/temporary switching and activation are O(1); no allocation on the
  hot event path beyond small interaction buffers.

## Edge cases & failure modes

- **Tool switch mid-drag** — `deactivate` commits the valid in-flight edit or
  cancels; never leaves a partial command or a dangling preview overlay.
- **No active layer / locked layer / hidden layer** — a tool that needs a pixel
  target checks first and either no-ops with a status hint or targets a valid
  alternative (e.g. paint requires an editable raster layer; otherwise warn, push
  nothing).
- **Click with no movement vs. drag** — tools must handle a zero-length
  interaction (a single brush dab; a magic-wand click; a point-add for the Pen)
  distinctly from a drag.
- **Right-click / context** — reserved for the shell's context menu unless a tool
  explicitly uses the secondary button; tools must not assume left-only.
- **Events outside the canvas / off-document** — legal (layers extend past the
  canvas); coordinates may be negative. Navigation tools clamp pan/zoom to sane
  limits.
- **Modifier changes mid-drag** — e.g. holding Shift to constrain after starting:
  tools read live `modifiers()` each event, not just at `Down`.
- **Rapid tool cycling via shortcut** — `ToolManager` cycles tools sharing a key
  deterministically; activation is idempotent.
- **Pen on a mouse (no pressure)** — `pressure = 1`; tilt = 0; pressure-driven
  dynamics fall back gracefully (handled in the brush engine).

## Testing strategy

Headless `pe_core` tests (no window needed — the whole point of the split):

- **One-command invariant** — synthesize Down→Move…→Up for each interactive tool
  and assert exactly one command lands on the history with the expected `name`
  and dirty region.
- **Activate/deactivate** — switching tools mid-interaction commits-or-cancels
  cleanly; no leaked overlay state; options observers fire correctly.
- **Coordinate transforms** — `docToScreen`/`screenToDoc` round-trip; handle
  hit-testing at multiple zooms/rotations picks the right handle.
- **Options model** — `set(id, value)` clamps to range, notifies observers, and
  serializes/restores as a tool preset.
- **Spring-loaded** — `pushTemporary`/`popTemporary` restores the prior tool and
  its in-flight state.
- **Navigation tools push no command** — Hand/Zoom/Rotate-View mutate only the
  viewport (assert history unchanged).
- **Category coverage smoke** — each canonical tool registers, activates, and
  produces its expected command type on a scripted interaction.

## Phasing

- **M3 (this doc lands)** — `Tool`/`ToolContext`/`ToolManager`/`ToolOptions`,
  overlays; Brush, Pencil, Eraser, Eyedropper, Paint Bucket, Gradient, Hand,
  Zoom, Move, basic Marquee; the one-command rule and spring-loaded switching.
- **M4** — full selection tool family (lasso/magic wand/quick/object) and the
  Move tool's selection interactions, wired to [selections](07-selection-system.md).
- **M5/M8** — Crop, Transform (basic in M3 → full in M8), Pen/Shape/Path
  selection ([vector](18-vector-paths.md)), Type tools ([text](17-text-typography.md)).
- **M9** — retouching tools ([retouching](13-retouching.md)) and AI tools
  ([generative AI](14-generative-ai.md)).
- **M10** — plugin-contributed tools via [plugins](29-plugin-extension.md); tool
  presets matured in [presets](25-presets-assets.md).

## Open questions

- **Command merging window** — who owns the policy for coalescing a multi-drag
  transform into one history entry: the tool, or the [history](21-history-undo.md)
  system? Leaning history-defined with the tool opting in via `mergeWith`.
- **Modal vs. modeless transform** — should Free Transform be a true modal state
  (Enter/Esc to confirm/cancel) or a regular tool with an on-canvas confirm? Modal
  matches expectations but complicates tool switching.
- **Right-button gestures** — reserve entirely for the shell context menu, or let
  specific tools (e.g. brush-size scrub) claim it?
- **Per-tool vs. shared options** — should color/blend-mode be shared options
  across painting tools (so they persist when switching) or per-tool? Photoshop
  shares some; we likely mirror that.
- **Touch/gesture input** — pinch-zoom/two-finger-pan belong to navigation; do
  they live in the tool layer or as shell-level gestures bypassing the active
  tool?

## References (relative links)

- [01 — Master architecture](../01-master-architecture.md) — the `Tool` and
  `Command` contracts and the input→command data flow.
- [00 — Vision & scope](../00-vision-and-scope.md) — headless engine; the UI is a
  client of the engine.
- [Glossary](../glossary.md) — Command, Selection, Layer, Non-destructive.
- Sibling systems: [08 — Brush engine](08-brush-engine.md),
  [07 — Selection system](07-selection-system.md),
  [10 — Transform system](10-transform-system.md),
  [13 — Retouching](13-retouching.md), [18 — Vector & paths](18-vector-paths.md),
  [17 — Text & typography](17-text-typography.md),
  [14 — Generative AI](14-generative-ai.md),
  [02 — Canvas & rendering](02-canvas-rendering.md),
  [24 — UI & workspace](24-ui-workspace.md),
  [25 — Presets & assets](25-presets-assets.md),
  [21 — History & undo](21-history-undo.md),
  [26 — Automation](26-automation.md), [29 — Plugins](29-plugin-extension.md).
- ADRs: [0005 — command/history model](../adr/0005-command-history-model.md),
  [0006 — headless core](../adr/0006-headless-core-separation.md).
