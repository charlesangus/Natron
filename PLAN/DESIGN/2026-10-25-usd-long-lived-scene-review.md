# USD long-lived scene review (2026-10-25)

Thorough review of M19 - USD/Hydra Foundation, M20 - 3D Node Vocabulary and the planned M52-M54 against the installed USD 0.26.8 headers, OpenUSD v26.08 source, usdview and the USD docs. VERIFIED claims cite headers/source; RECOLLECTION claims are unverified.

**Recommendation:** go ahead with the long-lived-scene redesign, but keep M19's immutable per-node layers. Each consumer (viewer, each USDRender, and a small pool of query stages) owns one long-lived `UsdStage` on a root layer only it can edit. Its sublayer list is updated in place to point at the new node layers, and it has one persistent engine. The implementer's claim that rebuilding was unavoidable is half right: an engine does bind one stage for life (VERIFIED engine.cpp:534-556 at v26.08; the installed USD is 0.26.8). But nothing forces a new stage per edit, and USD propagates changes to an existing stage incrementally (VERIFIED below).

**(a) Summary.** We now build a new stage, a new engine and a new Hgi on every edit. Viewer: `/home/bosley/git/Natron/.worktrees/m20/Gui/SceneViewport.cpp:336-352`. USDRender: `.../m20/Engine/Nodes/Scene/HydraRenderImpl.cpp:571-587`. A fresh engine creates its own Hgi (VERIFIED engine.cpp:2265-2267) and repopulates and re-uploads the whole scene. A probe on the container's USD, with 20k prims and 40 sublayers, measured:
- a fresh stage open plus traverse: 97 ms, before Hydra repopulates anything;
- swapping one sublayer on a live stage: 12 ms;
- an in-place value edit: 0.2 ms;
- `TransferContent`: 0.3 ms.

The swap's notices named only the changed paths, never `/`. UsdImaging turns property resyncs into invalidations rather than repopulation (VERIFIED stageSceneIndex.cpp:527-545) and applies queued changes on the next render (engine.cpp:2372). Immutable layers stay; the per-edit stage and engine go.

**(b) Findings** (Sev = severity; B = blocker, M = major, m = minor; paths under `.worktrees/m20/` unless noted)

| Dim | Sev | Finding | Evidence | Change |
|---|---|---|---|---|
| 2 | B | Every edit builds a new stage and engine (viewer and USDRender) | as above; VERIFIED | Long-lived stage and engine per consumer |
| 2,7 | B | Nodes that read their upstream (Transform, Prune, Material, USDRender) each build an upstream stage from a shared 16-entry cache. A 30-node chain thrashes it and costs O(N²) composition per edit | `Engine/Nodes/Scene/StageCache.cpp:34`, `USDTransform.cpp:241,350`, `USDPrune.cpp:102`, `USDMaterial.cpp:332`, `USDRender.cpp:182,303,365` | Query-stage pool (c) |
| 1 | M | A node's layer key includes the whole upstream stack hash, so any upstream edit re-authors every downstream layer, with animated knobs baked over the full range | `SceneOps.cpp:220-226`; `SceneNodeAuthoring.cpp:238-242` | Key only on what the node actually reads (matched paths and values read) |
| 5 | B | Reload. M19 head reopens the top file under a stamp argument, but nested sublayers and references stay stale while any stage holds them. Probe: radius stayed 1 after `b.usda` changed on disk. Also, only the top file's mtime is hashed | m19 `USDRead.cpp:224`; VERIFIED probe | Real `SdfLayer::ReloadLayers` (layer.h:390; `Reload` checks mtime and external dependencies itself, layer.h:369-381), behind a write barrier (c) |
| 3 | B | In-place reloads while render threads may be reading. `UsdStage::Reload` must only be called when no thread reads any stage (VERIFIED stage.h:411-421; openusd.org threading page: "multiple readers or a single writer", and a layer shared by two stages makes them one write domain) | `USDWrite.cpp:597` (`open->Reload(true)`); `USDRead.cpp:200`, where m20 has not been restacked on M19 head | Barrier |
| 3 | M | Stage-change queues inside the stage scene index have no lock. Edits and `Render` on the same stage must be serialized | VERIFIED stageSceneIndex.cpp:510-599 | Each consumer edits its stage only on, or under the lock of, its render thread |
| 4 | M | Each USDRender engine gets its own Hgi and texture registry on the one Hydra thread | engine.cpp:385 / 2265; engine.h:95 (`Parameters::driver`) | One shared HgiGL per GL context, passed through `HdDriver` |
| 1 | M | USDMerge's diamond handling is right in code (a shared layer keeps its weakest position), but the plan text says "strongest". A downstream Append-mode Transform authors a full `xformOpOrder` computed from its own branch's upstream, which can drop the other branch's ops after merge (RECOLLECTION that Solaris's separate-layers merge has the same caveat) | `USDMerge.cpp:55-92`; M20 plan line 81 | Fix the plan text; give Transform an Append mode that is safe under merge, or warn |
| 5 | M | USDWrite layered mode moves every external file into the output's sublayers, below all flattened Natron opinions. A file read downstream of an authored layer flips strength | `USDWrite.cpp:527-577` | Keep stack order: flatten each run of consecutive anonymous layers in place, between the externals |
| 5 | m | Relative asset paths in anonymous layers resolve against the process working directory | `USDWrite.cpp:541-546` comment | Author anchored absolute paths; relativize only on write |
| 4 | m | Stage opened with LoadAll, no load rules or population mask; payload control comes later | `SceneOps.cpp:178` | Viewer load rules, `UsdStage::SetLoadRules` |
| 1 | m | Root metadata such as `upAxis` is plugin metadata (RECOLLECTION); a change on `/` resyncs everything (VERIFIED stageSceneIndex.cpp:556-567) | `SceneOps.cpp:167-175` | Write root metadata only when it changes |

**(c) Target architecture** (APIs checked against the installed headers)
- **Node layers:** as today, one immutable anonymous layer per node, memoized. A `ScenePayload` is still the stack. Immutability keeps M177's render isolation: a request carries the exact stack it saw.
- **`SceneStage`** (new): owns a private root layer, a `UsdStage::Open(root, SdfLayerHandle(), load)` (stage.h:330) and the stack it last applied. `sync(payload)` diffs the stacks and calls `SetSubLayerPaths` (layer.h:1224) inside an `SdfChangeBlock` (changeBlock.h:56), so only changed paths are notified.
- **Viewer:** one `SceneStage` and one `UsdImagingGLEngine` per viewer, for the widget's lifetime. The scene-index path is already the default (engine.cpp:78). Sync runs under a viewer mutex, on a worker or the GUI thread. Paint skips the redraw if the lock is held. Selection and camera survive edits.
- **USDRender:** on the Hydra thread, one `SceneStage` and engine per node, with the Hgi shared through `Parameters::driver`. Each job runs `sync(request.payload)`, then `Render(..., params.frame)`. Frames with the same stack don't re-sync; time changes go through `SetTime`.
- **Query pool:** about one stage per render thread, borrowed exclusively and synced to the needed upstream stack. Probe: adding one sublayer cost 0.7 ms. Replaces every `StageCache::getStage`; StageCache is deleted.
- **Reload:** goes through a global scene write barrier: abort or drain renders, idle the Hydra thread, pool stages returned. Then `SdfLayer::ReloadLayers` over the file's closure (`UsdStage::GetUsedLayers`, or `GetCompositionAssetDependencies`, layer.h:425). Notices reach every live stage, so no format-argument keying is needed. USDWrite's re-read of its own output goes through the same barrier. This is usdview's model: Reload in place and keep the engine; drop the engine only for a new stage (VERIFIED appController.py:3026-3036, 1304-1309).

**(d) Phases**
- **M19 redesign**
  - T1 `SceneStage` plus incremental sync, with notice-count tests (M).
  - T2 viewer on a persistent engine; delete `ensureEngine`'s rebuild (M).
  - T3 query pool replaces StageCache in nodes (M).
  - T4 write barrier and real Reload, with a nested-sublayer test (L).
  - T5 finer layer keys (M).
  - T6 perf gate: a 30-node chain and a 1M-prim file, edit-to-frame latency (M).
- **M20 redesign**
  - Restack on M19 head (S).
  - USDRender per-node `SceneStage` and shared Hgi (M).
  - USDWrite ordered flattening, and its re-read through the barrier (M).
  - Merge plan text and Transform Append under merge (S).
- **M52:** `selectPrims` and USDVariant use the query pool (S). USDReference's "strongest layer by anonymous identifier" (plan line 60) drops the weaker layers: reference a stack layer that sublayers the whole input, and handle it in USDWrite (M).
- **M53:** drop "the viewport re-composes on stack hash" (P2 viewport task). A `?v=` path change is a plain property and asset-path invalidation (stageSceneIndex.cpp:586-595) (S). The `natron://` resolver must not render synchronously from inside Hydra on the GUI thread, or it risks deadlock (RECOLLECTION on Hio's loading path; check it) (M).
- **M54:** P2.T4's scratch stage "keyed by detail hash" would rebuild per frame. Use a viewer-private geometry layer edited in place instead (value-only changes become dirty points) (M). GeoToUSD keys as T5.

**(e) Open questions**
1. Viewer sync off the GUI thread (worker plus lock) or on it? Off costs more code but avoids hitches on huge resyncs.
2. Reload barrier: abort in-flight renders, or wait for them?
3. Per-USDRender engines, or one engine per Hydra thread shared by all nodes? Sharing saves GPU memory but makes nodes switch stacks back and forth.
4. Should M54's geometry key on time? Each scrubbed frame then costs a re-author.

**Not settled:** Solaris and HUSD internals. sidefx.com was unreachable, so any Houdini comparison is RECOLLECTION. Also not measured: timings at 1M-prim scale, and Hydra repopulate cost (only USD-side costs were probed).