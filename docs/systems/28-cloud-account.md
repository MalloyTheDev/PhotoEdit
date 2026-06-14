# 28 — Cloud / Account

> Milestone: M10 (optional platform layer) · Status: Spec

## Purpose

Modern professional editors connect to cloud services: account sign-in and
entitlement, cloud documents, shared asset libraries, AI service routing, settings
sync, collaboration boards, and stock integration. Per the
[vision](../00-vision-and-scope.md), **cloud is not needed initially** and is an
optional later platform layer — PhotoEdit is fully usable offline. This spec
defines the boundaries so that, when added, cloud features plug into the existing
engine without compromising the offline-first, privacy-respecting design.

## Requirements

**Functional**

- Account sign-in / `AuthSession`; subscription/entitlement checks that gate
  premium or service-backed features.
- **Cloud documents**: open/save the [native format](20-file-io.md) to cloud
  storage with conflict handling and offline caching.
- **Asset libraries**: shared [presets/assets](25-presets-assets.md) synced across
  devices.
- **AI service routing**: provide cloud [generative](14-generative-ai.md) providers
  and route requests with auth/quota.
- Settings sync, collaboration "boards" (share a document to a shared space),
  stock asset search/placement.

**Non-functional**

- **Offline-first**: every non-cloud feature works with no network/sign-in.
- **Privacy/consent**: uploading content (documents, AI context) requires explicit,
  revocable consent; clearly disclose what leaves the device.
- The networking lives in an isolated module; `pe_core` image logic stays
  network-free ([ADR-0006](../adr/0006-headless-core-separation.md)).

## Data model

```cpp
namespace pe {

struct AuthSession { Uuid userId; Token token; Entitlements entitlements; bool offline; };

class CloudDocumentStore {
public:
    JobHandle open(CloudDocId, std::function<void(Result<Document>)>);
    JobHandle save(const Document&, CloudDocId, ConflictPolicy);
    bool isCachedOffline(CloudDocId) const;
};

class AssetLibraryClient { /* list/pull/push preset libraries */ };

struct ConsentState { bool allowCloudAI = false; bool allowCloudDocs = false; };

} // namespace pe
```

## Behavior & algorithms

- **Auth**: a sign-in flow yields an `AuthSession` with cached `Entitlements`;
  features query entitlements, degrading gracefully when offline or unentitled.
- **Cloud documents**: save uploads the native document (chunked, resumable);
  conflicts (concurrent edits) resolve by policy (keep-both, last-writer, or merge
  where the model allows). A local cache keeps recent cloud docs available offline.
- **AI routing**: a cloud `GenerativeProvider` (see [14](14-generative-ai.md))
  attaches auth + quota to requests; on failure it falls back to a local provider
  or classical tools.
- **Consent gating**: any operation that would transmit user content checks
  `ConsentState` first and surfaces exactly what will be sent.

## Interactions

- [Generative AI](14-generative-ai.md): cloud model providers route through here.
- [File I/O](20-file-io.md) / [document system](01-document-system.md): cloud is an
  alternate document store for the native format.
- [Presets/assets](25-presets-assets.md): library sync.
- [Automation](26-automation.md): batch over cloud documents (later).

## Performance, threading & GPU

- All network I/O is async via the [job system](22-performance.md); the UI never
  blocks on the network.
- Uploads/downloads are chunked, resumable, and cancellable; offline cache avoids
  redundant transfers.

## Edge cases & failure modes

- Offline / signed-out → all local features work; cloud features show a clear,
  non-blocking unavailable state.
- Token expiry mid-operation → transparent refresh or queued retry.
- Entitlement lapse → disable service features, keep the user's local data intact.
- Consent revoked → immediately stop transmitting; purge server-side per policy.
- Sync conflict → never silently lose edits; present resolution.

## Testing strategy

- All tested against a **mock backend**; no live network in CI.
- Unit: entitlement gating; consent gating blocks transmission when disabled.
- Conflict-policy resolution; offline cache hit/miss; resumable upload after an
  injected interruption.

## Phasing

- **M10 (optional)**: auth/entitlement, cloud documents with offline cache, AI
  cloud routing, basic library sync.
- **Post-M10**: collaboration boards, stock integration, settings sync.

## Open questions

- Backend/storage provider and protocol; document chunk/resumable format.
- Collaboration model (async share vs eventual live co-edit — live is a non-goal).
- Content-credential/provenance storage in the cloud (with [AI](14-generative-ai.md)).

## References (relative links)

- [00 — Vision & scope](../00-vision-and-scope.md) — cloud is optional/later;
  offline-first.
- [Glossary](../glossary.md) — App shell.
- Sibling systems: [14 — Generative AI](14-generative-ai.md),
  [25 — Presets/assets](25-presets-assets.md), [20 — File I/O](20-file-io.md),
  [01 — Document system](01-document-system.md), [26 — Automation](26-automation.md).
- ADRs: [0006 — headless core](../adr/0006-headless-core-separation.md).
