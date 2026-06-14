# PhotoEdit Documentation

This is the complete design for PhotoEdit, a flagship-level, Photoshop-class
image editor. The documents are the blueprint; code is built against them, in
the milestone order defined by the [roadmap](03-roadmap-and-milestones.md).

## How to read this

If you are new, read in this order:

1. [Vision & Scope](00-vision-and-scope.md) — what we are building and why,
   what's in and out, and the principles every decision answers to.
2. [Master Architecture](01-master-architecture.md) — the whole system on one
   page: modules, data flow, the compositor, threading, and how every
   subsystem plugs in.
3. [Technology Stack](02-technology-stack.md) — C++/Qt6, GPU strategy,
   dependencies, and the reasoning.
4. [Roadmap & Milestones](03-roadmap-and-milestones.md) — the build order, M0→M10.

Then dive into any [system specification](#system-specifications) as needed.

## Foundational documents

| Doc | Purpose |
| --- | --- |
| [00 — Vision & Scope](00-vision-and-scope.md) | Goals, non-goals, principles, target user |
| [01 — Master Architecture](01-master-architecture.md) | Top-level system design and data flow |
| [02 — Technology Stack](02-technology-stack.md) | Language, framework, GPU, libraries |
| [03 — Roadmap & Milestones](03-roadmap-and-milestones.md) | Phased delivery plan M0→M10 |
| [04 — Coding Standards](04-coding-standards.md) | C++ style, error handling, testing bar |
| [05 — Build & Tooling](05-build-and-tooling.md) | CMake, vcpkg, CI, formatting, sanitizers |
| [Glossary](glossary.md) | Shared vocabulary |
| [ADRs](adr/README.md) | Architecture Decision Records (the "why") |

## System specifications

Each document specifies one engine system: its purpose, data model (as concrete
C++ shapes), algorithms, interactions with other systems, performance/GPU notes,
edge cases, and the milestone it lands in.

| # | System | Lands in |
| --- | --- | --- |
| [01](systems/01-document-system.md) | Document system | M1 |
| [02](systems/02-canvas-rendering.md) | Canvas & rendering (tile renderer) | M2 |
| [03](systems/03-layer-system.md) | Layer engine | M1 |
| [04](systems/04-blend-modes.md) | Blend modes | M1 |
| [05](systems/05-adjustment-layers.md) | Adjustment layers | M5 |
| [06](systems/06-masks.md) | Masks | M4 |
| [07](systems/07-selection-system.md) | Selection system | M4 |
| [08](systems/08-brush-engine.md) | Brush engine | M3 |
| [09](systems/09-tool-system.md) | Tool system | M3 |
| [10](systems/10-transform-system.md) | Transform system | M3/M8 |
| [11](systems/11-smart-objects.md) | Smart objects | M8 |
| [12](systems/12-filter-engine.md) | Filter engine & smart filters | M5 |
| [13](systems/13-retouching.md) | Retouching / restoration | M9 |
| [14](systems/14-generative-ai.md) | Generative AI system | M9 |
| [15](systems/15-color-management.md) | Color management | M6 |
| [16](systems/16-camera-raw.md) | Camera Raw / photography pipeline | M8+ |
| [17](systems/17-text-typography.md) | Text & typography | M8 |
| [18](systems/18-vector-paths.md) | Vector & path system | M8 |
| [19](systems/19-channels.md) | Channels | M6 |
| [20](systems/20-file-io.md) | File import/export | M7 |
| [21](systems/21-history-undo.md) | History, undo & snapshots | M3 |
| [22](systems/22-performance.md) | Performance (memory, scratch, threads) | M2+ |
| [23](systems/23-gpu-acceleration.md) | GPU acceleration | M2+ |
| [24](systems/24-ui-workspace.md) | UI / workspace | M0+ |
| [25](systems/25-presets-assets.md) | Presets & assets | M3+ |
| [26](systems/26-automation.md) | Automation (actions, batch, scripts) | M10 |
| [27](systems/27-printing-prepress.md) | Printing / prepress | M10 |
| [28](systems/28-cloud-account.md) | Cloud / account | M10 |
| [29](systems/29-plugin-extension.md) | Plugin / extension system | M10 |

## Document conventions

- **Code shapes** are illustrative C++ that communicates intent; the real
  headers may differ in detail but not in concept.
- **Normative language**: "must" = required for correctness; "should" =
  strong default; "may" = optional.
- When a design choice is significant or contested, it gets an
  [ADR](adr/README.md) and the system doc links to it rather than re-arguing it.
