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
| Node content hash | `Node::computeContentHash` Engine/Node.cpp:950 | Plug-in and binary stamp, render-hashed knobs, inputs, roto, groups, frame range, project digest | [keys.md](cache-invalidation/keys.md) | suspected hole (K3, K4, K5, K7, K8; K1 and K2 fixed by M202) |
| Frame args `nodeHash` (render hash) | `ParallelRenderArgsSetter::buildArgsMap` Engine/ParallelRenderArgs.cpp:1944 | Content hash mixed with the render-context key | [keys.md](cache-invalidation/keys.md) | fixed by M202 (K1, K2) |
| Project digest | `Project::getContentDigest` Engine/Project.cpp:3460 | Project knobs, formats, layers, OCIO, concatenation generations | [keys.md](cache-invalidation/keys.md) | suspected hole (K7: Settings outside the digest) |
| Upstream context read-set | `Node::getUpstreamContextReadSet` Engine/NodeInputs.cpp:2101 | `getRenderContextReadsGeneration()` | [keys.md](cache-invalidation/keys.md) | covered |
| Topology edits (inputs, queued edits) | Engine/NodeInputs.cpp, `Node::dequeueActions` Engine/Node.cpp:7931 | Input slots folded into the content hash | [topology.md](cache-invalidation/topology.md) | T1 (queued edits), T2 and T6 fixed in M242; T1 also gated by M202; suspected hole (T3, T4, T5, T7) |
| ActionsCache: identity | `ActionsCache` Engine/EffectInstancePrivate.h:216 | (render hash, time, view, mipmap) | [actions-and-metadata.md](cache-invalidation/actions-and-metadata.md) | A1 fixed in M242; A2 fixed by M202 |
| ActionsCache: RoD, frames needed, time domain, components needed | Engine/EffectInstancePrivate.h:252-268 | (render hash, time, view[, mipmap]) | [actions-and-metadata.md](cache-invalidation/actions-and-metadata.md) | fixed by M202 (A2); A5 latent |
| ActionsCache: metadata, pass-through, time-invariant, range summary | Engine/EffectInstancePrivate.h:274-308 | (hash with context key, time, view) plus generation | [actions-and-metadata.md](cache-invalidation/actions-and-metadata.md) | suspected hole (A4: non-abortable render uses newer metadata) |
| NativeEffectBase output metadata | `NativeEffectBase::getOutputMetadata` Engine/Nodes/NativeEffectBase.cpp:387 | (time, view) under one node hash | [actions-and-metadata.md](cache-invalidation/actions-and-metadata.md) | fixed by M202 (A3) |
| Effective output data kind | `Node::getEffectiveOutputDataKind` Engine/Node.cpp:2016 | Memo per node, cleared by `invalidateEffectiveOutputDataKindCache` Engine/Node.cpp:2377 | [actions-and-metadata.md](cache-invalidation/actions-and-metadata.md) | covered |
| NodeCache | `Cache<Image>` "NodeCache" Engine/AppManager.cpp:1041 | `ImageKey` (Engine/ImageKey.h:42-51) | [image-caches.md](cache-invalidation/image-caches.md) | I1 fixed in M242; suspected hole (I4); I2 fixed by M202 |
| DiskCache | `Cache<Image>` "DiskCache" Engine/AppManager.cpp:1042 | `ImageKey`, persisted under `NATRON_CACHE_VERSION` | [image-caches.md](cache-invalidation/image-caches.md) | I1 fixed in M242; suspected hole (I3); I2 fixed by M202 |
| DeepImageCache | `Cache<DeepImageCacheEntry>` Engine/AppManager.cpp:1046 | `DeepImageKey` (Engine/DeepImageKey.h:46-49) | [image-caches.md](cache-invalidation/image-caches.md) | fixed by M202 (I2) |
| FrameStore | `FrameStore` Engine/FrameRenderContext.h:58 | `TaskKey` (node, time, view, mipmap, context key) | [image-caches.md](cache-invalidation/image-caches.md) | covered |
| DiskCache node | `DiskCacheNode` Engine/DiskCacheNode.cpp | `ImageKey` in DiskCache | [image-caches.md](cache-invalidation/image-caches.md) | I1 fixed in M242; K2 frame range fixed by M202 |
| ViewerCache | `Cache<FrameEntry>` Engine/AppManager.cpp:1043; stores in Engine/ViewerInstance.cpp | `FrameKey` (Engine/FrameKey.h:125-140) | [viewer-and-timeline.md](cache-invalidation/viewer-and-timeline.md) | fixed by M202 (V1) |
| Viewer on-screen images | `TextureInfo::lastRenderedTiles` Gui/ViewerGLPrivate.h:148-165 | Per mipmap level | [viewer-and-timeline.md](cache-invalidation/viewer-and-timeline.md) | V4 fixed in M242 |
| Timeline cached-frames line | `TimeLineGui::refreshCachedFrames` Gui/TimeLineGui.cpp:1436 | (viewer hash, mipmap level, holder), any tile | [viewer-and-timeline.md](cache-invalidation/viewer-and-timeline.md) | V5 fixed in M242; suspected hole (V2, V3; display only) |
| GpuTextureCache | Engine/Gpu/GpuTexture.h:91 | Caller string; OCIO hashes description and texels | [gpu.md](cache-invalidation/gpu.md) | covered (keyed by content; no keying test) |
| GpuPipelineCache | Engine/Gpu/GpuPipelineCache.h:39 | Versions, device tag, stage, entry, source | [gpu.md](cache-invalidation/gpu.md) | covered |
| GPU host import cache | Engine/Gpu/GpuTransfer.cpp:347 | Host address and length | [gpu.md](cache-invalidation/gpu.md) | suspected hole (G1) |
| GPU device residency, StripMemory, GpuBufferPool | Engine/GpuRegionTask.cpp, Engine/Gpu/GpuBufferPool.h | `TaskKey`; tuning and size classes hold no content | [gpu.md](cache-invalidation/gpu.md) | covered |
| Expression results (`_exprRes`, `_exprCtxRes`) | Engine/Knob.h:2859 | time; (time, view, context key) | [expressions-and-links.md](cache-invalidation/expressions-and-links.md) | suspected hole (E3 minor) |
| Expression dependencies in the content hash | `getDimensionContentHashRecursive` Engine/Knob.cpp:6755 | Dependencies, masters, metadata sources | [expressions-and-links.md](cache-invalidation/expressions-and-links.md) | suspected hole (E1, fixed by M72 not M202; metadata residual) |
| Links (link groups, master/slave) | Engine/KnobLinkGroup.h:60 | No result cache | [expressions-and-links.md](cache-invalidation/expressions-and-links.md) | covered |
| SceneLayerMemo | Engine/Nodes/Scene/SceneLayerMemo.h:40 | Node digest, upstream stack or facts, frame range, fps | [node-private.md](cache-invalidation/node-private.md) | suspected hole (N1) |
| USDRead closure | Engine/Nodes/Scene/USDRead.cpp:81 | `closureDigest` | [node-private.md](cache-invalidation/node-private.md) | suspected hole (N2) |
| OIIO header and Raw decode caches | Engine/Nodes/IO/OiioReadSupport.cpp:63, Engine/Nodes/IO/RawDecodeSupport.cpp:78 | (path, mtime, size) | [node-private.md](cache-invalidation/node-private.md) | suspected hole (N3, deferred: needs a design decision) |
| StageGpuShaderGate | Engine/Nodes/Image/StagePlan.h:415 | Entry and source, no device | [node-private.md](cache-invalidation/node-private.md) | suspected hole (G2; placement only) |
| Other node-private caches (CurveTablesCache, NativeRead listings, NativeWrite read-back, RawDemosaic memos, AddLayers signature, USDTransform matches, DeepRead layers, IErodeDilate) | Engine/Nodes | Content hash or file identity; see the doc | [node-private.md](cache-invalidation/node-private.md) | covered (CurveTablesCache inherits E1) |

## Stress tests

`Tests/CacheStress.h` drives seeded edit walks over small graphs and compares every cached render,
bit for bit in Legacy and in Task graph, with a render of the same state from empty caches. A
failure prints the seed, the step and the op log; `NATRON_CACHE_STRESS_SEED=<n>` replays one seed,
`NATRON_CACHE_STRESS_STEPS=<n>` changes the step budget and `NATRON_CACHE_STRESS_SEEDS=<n>` adds
seeds (the heavy mode). Suites: `CacheStressSelfTest`, `CacheStressTest`,
`CacheStressConcurrentTest` and `CacheStressGpuTest`, plus the GUI script
`Tests/gui/cache_invalidation_viewer.py`. Holes known open here have a `DISABLED_` case named after
the milestone that fixes them; run one with `--gtest_also_run_disabled_tests --gtest_filter=<name>`.
Cases for holes fixed here run by default:

| Case | Hole | State |
|---|---|---|
| `CacheStressTest.FrameExpression` | I1 | fixed in M242 |
| `CacheStressTest.ViewerABSwitch` | T2, V5 | fixed in M242 |
| `CacheStressTest.IdentityAcrossScales` | A1 | fixed in M242 |
| `CacheStressConcurrentTest.DequeueUnderARestartedRender` | T1, T6 | fixed in M242 |
| `CacheStressTest.DISABLED_WholeValueExpression_M72` | E1 | disabled until M72 |
| `CacheStressConcurrentTest.DISABLED_KnobEditsDuringRender_M202` | K1 | disabled until M202 (#131) |

Unit tests for the M242 fixes: `Tests/ExpressionFrameVarying_Test.cpp` (I1, E2) and
`Tests/QueuedInputEdit_Test.cpp` (T1, T6). V4 has no automated test.

## Follow-up list

Holes not fixed here and not owned by an open PR (M202 #131 owns K1, K2, A2, A3, I2, T1's gated
stores and V1; M72 owns E1). Details in the family docs.

| Hole | Summary | Doc |
|---|---|---|
| N3 | OIIO header and Raw decode caches re-read a changed file while the hash keeps its stamp | node-private.md |
| N1 | Scene layer stored under the key taken before authoring | node-private.md |
| N2 | USDRead sublayers changed while the application was closed | node-private.md |
| T3 | `setActiveInputAndRefresh` marks the viewer before it changes the active inputs | topology.md |
| T4 | `onInputChanged` runs under the pre-edit hash | topology.md |
| T5 | Group connection changes in the queue don't count as a change | topology.md |
| T7 | A direct input edit races a render that starts after the rendering check | topology.md |
| V2, V3 | Timeline cached-frames line out of date (display only) | viewer-and-timeline.md |
| G1 | GPU host import cache keyed by host address and length | gpu.md |
| G2 | Shader gate ignores the device | gpu.md |
| I3 | DiskCache persistence across holders | image-caches.md |
| I4 | Colour layers interchangeable on cache lookup | image-caches.md |
| E2 | Rename rewrites listeners by plain string replace (the bracket leak is fixed) | expressions-and-links.md |
| E3 | Clamped and unclamped expression results share entries | expressions-and-links.md |
| A4 | Non-abortable render uses newer metadata | actions-and-metadata.md |
| A5 | Same-hash drops skip geometry results (latent) | actions-and-metadata.md |
| K3-K8 | Content hash gaps: knobs outside the hash, parser-found dependencies, opt-in node state, frame range, Settings outside the digest, session stability | keys.md |
