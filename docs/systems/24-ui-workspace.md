# 24 — UI / Workspace

> Milestone: M0+ (continuous) · Status: Spec

## Purpose

The UI/workspace system is the Qt6 [app shell](../02-technology-stack.md): the
toolbar, options bar, canvas window, document tabs, and the family of dockable
panels (Layers, Properties, History, Color, Swatches, Brush Settings, Channels,
Paths, Actions, Adjustments, Libraries, Navigator, Info, Character/Paragraph). It
is a whole desktop-app framework problem: customizable, dockable, persistable
workspaces with keyboard shortcuts and a contextual task bar. Critically, the UI
**observes the engine and issues [commands](21-history-undo.md)** — panels reflect
engine state and never own image data.

## Requirements

**Functional**

- Dockable, floatable, tabbable panels; saved/restored workspace layouts.
- Toolbar (tool selection), options bar (driven by the active tool's options),
  menus, and a contextual task bar.
- Document tabs / multi-document windows; the canvas viewport widget.
- Full keyboard-shortcut system, user-customizable; tool presets.
- High-DPI, theming (dark/light), and accessibility basics.

**Non-functional**

- This is the **one** place Qt is allowed ([ADR-0006](../adr/0006-headless-core-separation.md));
  it depends on `pe_core` and never the reverse.
- Panels update from engine notifications without polling; the UI thread never
  does heavy image work (dispatched to [workers](22-performance.md)).

## Data model

```cpp
namespace pe::app {

// Built on Qt's QMainWindow + QDockWidget (the M0 skeleton already creates
// placeholder docks in src/app/MainWindow.cpp).
class PanelManager {
public:
    void registerPanel(PanelId, PanelFactory);
    void dock(PanelId, Qt::DockWidgetArea);
    void floatPanel(PanelId);
    void saveWorkspace(const QString& name);     // serialize layout
    void restoreWorkspace(const QString& name);
};

// A panel observes engine state and emits commands; it holds no pixel data.
class Panel : public QDockWidget {
public:
    virtual void onDocumentChanged(const ChangeSet&) = 0;  // engine notification
};

class ShortcutManager { /* binding table, customizable, context-aware */ };

} // namespace pe::app
```

## Behavior & algorithms

**Observer flow (engine → UI):** the [Document](01-document-system.md) notifies
registered observers with a typed `ChangeSet` after every committed mutation. The
shell adapts these to Qt signals; each panel refreshes only what changed (the
Layers panel mirrors the [layer tree](03-layer-system.md), the History panel
mirrors the [command stack](21-history-undo.md), the Navigator shows the composite
thumbnail).

**Command flow (UI → engine):** a panel/menu/tool action constructs a `Command`
and pushes it through the document's history — the same path automation uses, so
anything doable in the UI is scriptable.

**Options bar** is generated from the active [tool](09-tool-system.md)'s options
model, so adding a tool option requires no bespoke UI wiring.

**Workspace persistence** serializes dock geometry/state (Qt's `saveState`) plus
PhotoEdit metadata (which panels, tool presets) to a named workspace; restore
reapplies it.

## Interactions

- [Tool system](09-tool-system.md): toolbar + options bar; cursors/overlays.
- [Layer system](03-layer-system.md), [history](21-history-undo.md),
  [channels](19-channels.md), [paths](18-vector-paths.md): their panels observe
  engine state.
- [Canvas/rendering](02-canvas-rendering.md): the viewport widget hosts the
  composite + overlays (marching ants, handles, brush cursor).
- [Presets](25-presets-assets.md): swatches/brushes/tool presets panels.
- [Performance](22-performance.md): heavy reactions dispatched off the UI thread.

## Performance, threading & GPU

- The UI thread only orchestrates; compositing/filtering happen on workers/GPU and
  results are marshaled back.
- Thumbnails and previews render at reduced resolution to keep panels responsive.

## Edge cases & failure modes

- Workspace from an older version → migrate or fall back to default layout.
- A panel for a not-yet-implemented system shows an empty/disabled state (the M0
  placeholders).
- High-DPI / multi-monitor scaling → correct dock and canvas scaling.
- Long operations → modal/non-modal progress with cancel; never freeze the UI.

## Testing strategy

- The engine side (notifications, command construction) is unit-tested headless;
  panels assert they emit the right command for an action and refresh on a given
  `ChangeSet`.
- Workspace serialize→restore round-trip.
- UI smoke/automation tests for docking and shortcuts (later, with Qt Test).

## Phasing

- **M0**: main window, menus, placeholder dock panels, status bar (done).
- **M2**: real canvas viewport; Layers/Navigator panels backed by the engine.
- **M3+**: tool options bar, brush/swatches panels, shortcuts.
- **Continuous**: each milestone fills in its panels; workspaces, theming,
  contextual task bar mature toward M10.

## Open questions

- Qt Widgets throughout vs selective QML for some panels.
- Theming system (stylesheets vs custom) and accessibility scope.
- Multi-window vs MDI document model.

## References (relative links)

- [02 — Technology stack](../02-technology-stack.md) — Qt usage rules.
- [Glossary](../glossary.md) — App shell, Command.
- Sibling systems: [09 — Tool system](09-tool-system.md),
  [03 — Layer system](03-layer-system.md), [21 — History](21-history-undo.md),
  [25 — Presets](25-presets-assets.md), [02 — Canvas/rendering](02-canvas-rendering.md),
  [22 — Performance](22-performance.md).
- ADRs: [0006 — headless core](../adr/0006-headless-core-separation.md),
  [0001 — language & framework](../adr/0001-language-and-framework.md).
