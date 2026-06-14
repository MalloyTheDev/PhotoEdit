# Architecture Decision Records

ADRs capture significant, hard-to-reverse decisions and the reasoning behind
them, so we don't re-litigate them and so newcomers understand the "why." Each
record is immutable once accepted; if we change our minds, we add a new ADR that
supersedes the old one (and mark the old one Superseded).

## Format

Each ADR has: **Status** (Proposed / Accepted / Superseded), **Context** (the
forces at play), **Decision** (what we chose), **Consequences** (the trade-offs),
and **Alternatives considered**.

## Index

| # | Title | Status |
| --- | --- | --- |
| [0001](0001-language-and-framework.md) | Language & application framework: C++20 + Qt 6 | Accepted |
| [0002](0002-gpu-abstraction.md) | Own a thin RHI; D3D12 first, Vulkan next | Accepted |
| [0003](0003-tile-based-engine.md) | Tile-based pixel storage & dirty-region rendering | Accepted |
| [0004](0004-color-management.md) | Color-managed pipeline on Little-CMS 2 | Accepted |
| [0005](0005-command-history-model.md) | Command-based mutation & tile-delta history | Accepted |
| [0006](0006-headless-core-separation.md) | Headless engine core, UI as a client | Accepted |
| [0007](0007-native-document-format.md) | A versioned native document format; PSD for interchange | Accepted |
