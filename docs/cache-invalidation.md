# Cache invalidation map

One row per cache or keyed state. Each family document lists the cache's key, what invalidates
it, what doesn't, the tests that cover it, and the suspected holes, with file:line evidence. The
docs record branch `milestone/m242-cache-invalidation-audit`. "Fixed by M202" refers to PR #131
(`origin/milestone/m202-clean-hash-cache-stores`), and the M71 notes refer to PR #122
(`origin/milestone/m71-rfv-render-context`).

Status values:

- **covered**: tests pin it and no hole is open.
- **suspected hole**: at least one hole is open on this branch and not fixed by M202.
- **fixed by M202**: every hole found is closed by PR #131.

| Cache | Owner | Key | Doc | Status |
| --- | --- | --- | --- | --- |
| Node content hash | `Node::computeContentHash` Engine/Node.cpp:950 | Plug-in and binary stamp, render-hashed knobs, inputs, roto, groups, frame range, project digest | [keys.md](cache-invalidation/keys.md) | covered: K1 and K2 fixed by M202; K4, K6 and K7 fixed in M260; K3 swept and K5 audited in M260, no instance found; K8 safe (misses only), no change |
| Frame args `nodeHash` (render hash) | `ParallelRenderArgsSetter::buildArgsMap` Engine/ParallelRenderArgs.cpp:1944 | Content hash mixed with the render-context key | [keys.md](cache-invalidation/keys.md) | fixed by M202 (K1, K2) |
| Project digest | `Project::getContentDigest` Engine/Project.cpp:3460 | Project knobs, formats, layers, OCIO, concatenation generations | [keys.md](cache-invalidation/keys.md) | K7 fixed in M260 |
| Upstream context read-set | `Node::getUpstreamContextReadSet` Engine/NodeInputs.cpp:2101 | `getRenderContextReadsGeneration()` | [keys.md](cache-invalidation/keys.md) | covered |
| Topology edits (inputs, queued edits) | Engine/NodeInputs.cpp, `Node::dequeueActions` Engine/Node.cpp:7983 | Input slots folded into the content hash | [topology.md](cache-invalidation/topology.md) | T1 (queued edits), T2, T6 and T7 fixed in M242; T1 also gated by M202; T3, T4, T5 and T8 fixed in M260 (T5: a removed GroupInput is no longer restored by the next dequeue; nothing queues group connection changes) |
| ActionsCache: identity | `ActionsCache` Engine/EffectInstancePrivate.h:216 | (render hash, time, view, mipmap) | [actions-and-metadata.md](cache-invalidation/actions-and-metadata.md) | A1 fixed in M242; A2 fixed by M202 |
| ActionsCache: RoD, frames needed, time domain, components needed | Engine/EffectInstancePrivate.h:252-268 | (render hash, time, view[, mipmap]) | [actions-and-metadata.md](cache-invalidation/actions-and-metadata.md) | fixed by M202 (A2); A5 fixed in M260 |
| ActionsCache: metadata, pass-through, time-invariant, range summary | Engine/EffectInstancePrivate.h:274-308 | (hash with context key, time, view) plus generation | [actions-and-metadata.md](cache-invalidation/actions-and-metadata.md) | suspected hole (A4: a non-abortable viewer or single-frame render uses newer metadata; the frame on disk does not arise, writes abort) |
| NativeEffectBase output metadata | `NativeEffectBase::getOutputMetadata` Engine/Nodes/NativeEffectBase.cpp:387 | (time, view) under one node hash | [actions-and-metadata.md](cache-invalidation/actions-and-metadata.md) | fixed by M202 (A3) |
| Effective output data kind | `Node::getEffectiveOutputDataKind` Engine/Node.cpp:2016 | Memo per node, cleared by `invalidateEffectiveOutputDataKindCache` Engine/Node.cpp:2377 | [actions-and-metadata.md](cache-invalidation/actions-and-metadata.md) | covered |
| NodeCache | `Cache<Image>` "NodeCache" Engine/AppManager.cpp:1041 | `ImageKey` (Engine/ImageKey.h:42-51) | [image-caches.md](cache-invalidation/image-caches.md) | I1 fixed in M242; I4 does not occur (`CacheStressTest.RgbThenRgbaRequest` passes, engine unchanged); I2 fixed by M202 |
| DiskCache | `Cache<Image>` "DiskCache" Engine/AppManager.cpp:1042 | `ImageKey`, persisted under `NATRON_CACHE_VERSION` | [image-caches.md](cache-invalidation/image-caches.md) | I1 fixed in M242; I3 fixed in M260; I2 fixed by M202 |
| Deep entries of NodeCache | `DeepImageCacheEntry` in `Cache<Image>` "NodeCache" | `DeepImageKey` (Engine/DeepImageKey.h:46-49) | [image-caches.md](cache-invalidation/image-caches.md) | fixed by M202 (I2) |
| FrameStore | `FrameStore` Engine/FrameRenderContext.h:58 | `TaskKey` (node, time, view, mipmap, context key) | [image-caches.md](cache-invalidation/image-caches.md) | covered |
| DiskCache node | `DiskCacheNode` Engine/DiskCacheNode.cpp | `ImageKey` in DiskCache | [image-caches.md](cache-invalidation/image-caches.md) | I1 fixed in M242; K2 frame range fixed by M202 |
| ViewerCache | `Cache<FrameEntry>` Engine/AppManager.cpp:1043; stores in Engine/ViewerInstance.cpp | `FrameKey` (Engine/FrameKey.h:125-140) | [viewer-and-timeline.md](cache-invalidation/viewer-and-timeline.md) | fixed by M202 (V1) |
| Viewer on-screen images | `TextureInfo::lastRenderedTiles` Gui/ViewerGLPrivate.h:148-165 | Per mipmap level | [viewer-and-timeline.md](cache-invalidation/viewer-and-timeline.md) | V4 fixed in M242 |
| Timeline cached-frames line | `TimeLineGui::refreshCachedFrames` Gui/TimeLineGui.cpp:1436 | (viewer hash, mipmap level, holder), any tile | [viewer-and-timeline.md](cache-invalidation/viewer-and-timeline.md) | V5 fixed in M242; V2 and V3 fixed in M260 |
| GpuTextureCache | Engine/Gpu/GpuTexture.h:91 | Caller string; OCIO hashes description and texels | [gpu.md](cache-invalidation/gpu.md) | covered (keyed by content; no keying test) |
| GpuPipelineCache | Engine/Gpu/GpuPipelineCache.h:39 | Versions, device tag, stage, entry, source | [gpu.md](cache-invalidation/gpu.md) | covered |
| GPU host import cache | Engine/Gpu/GpuTransfer.cpp:347 | Host address and length | [gpu.md](cache-invalidation/gpu.md) | G1 fixed in M260 |
| GPU device residency, StripMemory, GpuBufferPool | Engine/GpuRegionTask.cpp, Engine/Gpu/GpuBufferPool.h | `TaskKey`; tuning and size classes hold no content | [gpu.md](cache-invalidation/gpu.md) | covered |
| Expression results (`_exprRes`, `_exprCtxRes`) | Engine/Knob.h:2859 | time; (time, view, context key) | [expressions-and-links.md](cache-invalidation/expressions-and-links.md) | E3 fixed in M260 |
| Expression dependencies in the content hash | `getDimensionContentHashRecursive` Engine/Knob.cpp:6755 | Dependencies, masters, metadata sources | [expressions-and-links.md](cache-invalidation/expressions-and-links.md) | suspected hole (E1, fixed by M72 not M202; metadata residual); K4 fixed in M260 |
| Links (link groups, master/slave) | Engine/KnobLinkGroup.h:60 | No result cache | [expressions-and-links.md](cache-invalidation/expressions-and-links.md) | covered |
| SceneLayerMemo | Engine/Nodes/Scene/SceneLayerMemo.h:40 | Node digest, upstream stack or facts, frame range, fps | [node-private.md](cache-invalidation/node-private.md) | N1 fixed in M260 |
| USDRead closure | Engine/Nodes/Scene/USDRead.cpp:81 | `closureDigest` | [node-private.md](cache-invalidation/node-private.md) | N2 fixed in M260 |
| OIIO header and Raw decode caches | Engine/Nodes/IO/OiioReadSupport.cpp:63, Engine/Nodes/IO/RawDecodeSupport.cpp:78 | (path, `FileIdentity`), pinned to the stamp until Reload | [node-private.md](cache-invalidation/node-private.md) | N3 fixed in M260 |
| StageGpuShaderGate | Engine/Nodes/Image/StagePlan.h:415 | Device tag, entry and source | [node-private.md](cache-invalidation/node-private.md) | G2 fixed in M260 |
| Other node-private caches (CurveTablesCache, NativeRead listings, NativeWrite read-back, RawDemosaic memos, AddLayers signature, USDTransform matches, DeepRead layers, IErodeDilate) | Engine/Nodes | Content hash or file identity; see the doc | [node-private.md](cache-invalidation/node-private.md) | covered (CurveTablesCache inherits E1) |

## Stress tests

`Tests/CacheStress.h` drives seeded edit walks over small graphs and compares every cached render,
bit for bit in Legacy and in Task graph, with a render of the same state from empty caches. A
failure prints the seed, the step and the op log; `NATRON_CACHE_STRESS_SEED=<n>` replays one seed,
`NATRON_CACHE_STRESS_STEPS=<n>` changes the step budget and `NATRON_CACHE_STRESS_SEEDS=<n>` adds
seeds (the heavy mode). Suites: `CacheStressSelfTest`, `CacheStressTest`,
`CacheStressConcurrentTest` and `CacheStressGpuTest`, plus the GUI script
`Tests/gui/cache_invalidation_viewer.py`. Holes known open here have a `DISABLED_` case, listed below
with the milestone that fixes them; run one with `--gtest_also_run_disabled_tests --gtest_filter=<name>`.
Cases for holes fixed here run by default:

| Case | Hole | State |
|---|---|---|
| `CacheStressTest.FrameExpression` | I1 | fixed in M242 |
| `CacheStressTest.ViewerABSwitch` | T2, V5 | fixed in M242 |
| `CacheStressTest.IdentityAcrossScales` | A1 | fixed in M242 |
| `CacheStressConcurrentTest.DequeueUnderARestartedRender` | T1, T6 | fixed in M242 |
| `CacheStressTest.ReadPinnedUntilReload`, `CacheStressTest.RawReadPinnedUntilReload` | N3 | fixed in M260 |
| `CacheStressConcurrentTest.SceneKnobEditDuringAuthoring` | N1 | fixed in M260 |
| `USDReadTest.USDReadSublayerChangedWhileClosed` | N2 | fixed in M260 |
| `CacheStressConcurrentTest.OnInputChangedUnderTheNewHash` | T4 | fixed in M260 |
| `CacheStressConcurrentTest.ViewerABSwitchDuringRender` | T3 | fixed in M260 |
| `CacheStressTest.RgbThenRgbaRequest` | I4 | passes in M260; I4 does not occur, engine unchanged |
| `CacheStressTest.ExpressionOutsideRange` | E3 | fixed in M260 |
| `CacheStressTest.RenderSettingEdit` | K7 | fixed in M260 |
| `CacheStressTest.NonHashedKnobSweep` | K3 | fixed in M260 |
| `CacheStressTest.AddLayersRegistryChange` | K5 | fixed in M260 (audit: no new overrides) |
| `CacheStressGpuTest.DiskCacheSourcesAreNotImportedByAddress` | G1 | fixed in M260 |
| `CacheStressGpuTest.DeviceSwitchRetriesRejectedShader` | G2 | fixed in M260 |
| `CacheStressTest.DISABLED_WholeValueExpression_M72` | E1 | disabled until M72 |
| `CacheStressConcurrentTest.DISABLED_KnobEditsDuringRender` | K1 | disabled until M202 (#131) |

Unit tests for the M242 fixes: `Tests/ExpressionFrameVarying_Test.cpp` (I1, E2) and
`Tests/QueuedInputEdit_Test.cpp` (T1, T6, T7). V4 has no automated test.

Unit tests for the M260 fixes: `NodeContentHashTest` (E2 rename, K4 getattr reads, K6 range
edits, K7 Settings digest), `Knob_Test` (E3), `RawKindFrameworkTest` (A5), `ProjectYaml` and
`ClipboardYaml` cache-ID tests plus `CacheStressSelfTest` (I3), `HashInvariantsTest` (V2, and N3
across projects), `USDReadTest` (N2 missing sublayers, N3 across readers), `QueuedInputEditTest`
(T5, T8, the A/B switch) and `Tests/gui/m178_timeline_cache_line.py` (V3). The random walk gains `dynamicExpression` (K4).

## Follow-up list

Holes not fixed here and not owned by an open PR (M202 #131 owns K1, K2, A2, A3, I2, T1's gated
stores and V1; M72 owns E1). Details in the family docs.

| Hole | Summary | Doc |
|---|---|---|
| A4 | A non-abortable viewer or single-frame render can use metadata newer than its render (sequence writes abort, so the frame on disk does not arise) | actions-and-metadata.md |
| K8 | Failed `statx` on the binary only causes misses (safe, no change) | keys.md |
