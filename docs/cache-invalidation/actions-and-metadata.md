# Action results and per-frame metadata

State recorded on branch `milestone/m242-cache-invalidation-audit`. "Fixed by M202" means PR #131
(`origin/milestone/m202-clean-hash-cache-stores`) closes the entry. "M71" means PR #122
(`origin/milestone/m71-rfv-render-context`).

## Caches

| Cache | Owner | Lifetime |
| --- | --- | --- |
| `ActionsCache` | `class ActionsCache` EffectInstancePrivate.h:216, one per effect (`EffectInstance::Implementation`, constructed EffectInstancePrivate.cpp:688) | Until the effect dies; states evicted LRU |
| `NativeEffectBase` output metadata | `_metadataCache` NativeEffectBase.h:641, `getOutputMetadata` NativeEffectBase.cpp:387 | Until the node hash moves or an explicit drop |
| Effective output data kind | `effectiveDataKindCache*` NodePrivate.h:587-589, `Node::getEffectiveOutputDataKind` Node.cpp:2016 | Until `invalidateEffectiveOutputDataKindCache` |
| Metadata revision | `metadataRevision` NodePrivate.h:429, `Node::getMetadataRevision` Node.cpp:1840 | Counter; moved on same-hash metadata changes (Node.cpp:1828, 1835) |

## Key

### ActionsCache states

The cache holds one state (`ActionsCacheInstance`) per hash. States are kept in an LRU list
(`getOrCreateActionCache` EffectInstancePrivate.cpp:77). A hit splices the state to the back
(:82). Creating a state past `_maxInstances + _reservedInstances` drops the front one (:86-88).

- `_maxInstances` = `max(kActionsCacheMinStates, idealThreadCount * 2)`, with
  `kActionsCacheMinStates = 64` (EffectInstancePrivate.cpp:42, constructed at :688).
- `_reservedInstances` grows by one per `reserveState()` (EffectInstancePrivate.cpp:108). M178
  sizing: `ParallelRenderArgs.cpp:822-830` reserves one extra state per render context a node runs
  under in a frame, capped at `kContextCountWarningThreshold`, through
  `EffectInstance::reserveActionsCacheState` (EffectInstance.cpp:525). `releaseState()`
  (EffectInstancePrivate.cpp:116) gives it back and trims the extra states (:123).
- The hash passed in is the render hash. With render contexts in use it has the context key mixed
  in (`mixRenderContextKey`, keys.md), so each context gets its own state.
- `useHash` (EffectInstancePrivate.cpp:141) only splices an existing state to the back. It never
  creates or drops one.

`ActionKey` (EffectInstancePrivate.h:55) is (time, view, mipmapLevel). Per-result sub-keys:

| Result | get (h) | set (h) | Key within the state | Generation-checked store | Store gated by M202 |
| --- | --- | --- | --- | --- | --- |
| Identity | 250 | 252 | (time, view, mipmap) (EffectInstancePrivate.cpp:167, 198) | no | yes (isIdentity_public) |
| Components needed | 254 | 256 | (time, view), mipmap 0 (EffectInstancePrivate.cpp:234) | no | yes |
| RoD | 258 | 260 | (time, view, mipmap) | no | yes |
| Frames needed | 262 | 264 | (time, view, mipmap) | no | yes |
| Time domain (frame range) | 266 | 268 | one per state (EffectInstancePrivate.cpp:357, 374) | no | yes |
| Metadata | 274 | 280 | (time, view) | yes (EffectInstancePrivate.cpp:418) | yes |
| Pass-through identity | 287 | 292 | (time, view) plus the metadata pointer | yes (:475) | yes (getLayersPassThroughInput) |
| Time-invariant metadata | 294 | 296 | one per state | yes (:503) | yes |
| Range metadata summary | 302 | 308 | (view, min, max), at most `kMaxCachedRangeSummaries = 8` (:50) | yes (:541) | n/a (built from validated answers, EffectInstance.cpp:7143-7150) |
| Metadata generation | 310 (get) | 315 (`moveMetadataGeneration`) | one counter per cache | — | — |

Clears: `clearAll` (h:221, cpp:130; drops all states and moves the generation), `clearResults`
(h:233, cpp:152; one state, generation++), `clearComponentsNeededResults` (h:256, cpp:257; every
state), `clearMetadataResults` (h:320, cpp:574; metadata, pass-through, time-invariant and range
summaries in every state, generation++). Metadata frames per state are capped at
`kMaxCachedMetadataFrames = 32` (EffectInstancePrivate.cpp:47).

### Call sites (EffectInstance.cpp)

- `isIdentity_public` 4309: get 4325, set 4380. A non-identity is stored as
  `kIdentityCacheNotIdentity` (4321). Only used when `useIdentityCache` is true.
- `getRegionOfDefinition_public` 4425: get 4437. A failed action clears the state (4483). A null
  RoD is stored at 4490, the RoD at 4506.
- `getFramesNeeded_public` 4530: get 4537, set 4550.
- `getFrameRange_public` 4556: get 4565, set 4584.
- `getLayersPassThroughInput` 4890: snapshot 4922-4924. Cacheable only when `getRenderHash()` equals
  the up-to-date node hash (4927). Validated set 4941.
- `getComponentsNeededAndProducedShared` 5313: get 5319, set 5336, not while
  `isResolvingLayersPassThrough()`.
- `getMetadataOverRange` 7122: range summary keyed on `mixRenderContextKey(node->getHashValue(),
  contextKey)` (7133-7134); kept only if every answer belongs to that hash (7143-7150).
- `getMetadata` 7194/7205 → `getMetadataAttempt` 7225: reads the live node hash (7264) plus the
  context key. Stores only if validated: mark count, hash, `KnobContentWrite` snapshot and
  generation (7279-7281). Retries up to `kMaxMetadataDerivationAttempts = 3` (7016).
- `getTimeInvariantMetadata` 7288: keyed on the node hash alone, validated store 7327.
- `findRenderMetadata` 7334 and `getRenderMetadata(frameArgs, ...)` 7363: look the answer up under
  `frameArgs->nodeHash`.

## Invalidated by

- Hash change. `EffectInstance::onNodeHashChanged` (6174) calls `useHash(hash)` (6179) and
  `moveMetadataGeneration()` (6180). It then runs `onMetadataMayHaveChanged` →
  `Node::onMetadataMayHaveChanged` (Node.cpp:1803) → `refreshMetadataReaders(false)` (Node.cpp:1846),
  and clears expression results if the node may have knob expressions. Old states are not dropped;
  they age out, which lets an undo hit them again.
- `moveMetadataGeneration` (EffectInstancePrivate.cpp:566) removes nothing. It only makes every
  in-flight metadata, pass-through, time-invariant or range store that read the old generation
  refuse to store (418, 475, 503, 541). RoD, identity, frames-needed, time-domain and components
  stores do not check it.
- `clearActionsCache` (EffectInstance.cpp:4811 → `clearAll` 4813). Callers:
  `prepareForNextPaintStrokeRender` (Node.cpp:472, call 478), `purgeAllInstancesCaches`
  (Node.cpp:5784, call 5792), `setNodeDisabled` (Node.cpp:7672, call 7680),
  `refreshAllInputRelatedData` (Node.cpp:8273, call 8296), `getViewerArgsAndRenderViewer` for a
  roto-paint input (ViewerInstance.cpp:738, call 754), `NativeRead::purgeCaches` (NativeRead.cpp:606,
  call 631), `NativeWrite::purgeCaches` (NativeWrite.cpp:1564, call 1588) and
  `reloadOtherNodesReadingTheseFiles` (KnobFile.cpp:89, call 117).
- `dropCachedMetadata` (EffectInstance.cpp:7466): `clearMetadataResults` plus
  `clearComponentsNeededResults`. `dropCachedMetadataDownstream` (7475) applies it to the node and
  every output, from `reloadOtherNodesReadingTheseFiles` (KnobFile.cpp:123) and
  `OfxEffectInstance::requestDeferredEditsRefresh` (OfxEffectInstance.cpp:2161, call 2175).
- Same-hash metadata change: `NativeEffectBase::invalidateOutputMetadata` (NativeEffectBase.cpp:439,
  called from NativeRead.cpp:630 and NativeWrite.cpp:1587) → `Node::onMetadataChangedUnderSameHash`
  (Node.cpp:1809). That drops the native metadata cache and `dropCachedMetadata` on every node
  downstream, bumps their `metadataRevision`, and refreshes their metadata readers.
- Native metadata cache: dropped when the hash it was filled under differs from the live hash
  (NativeEffectBase.cpp:400-403), on `onNodeHashChanged` (465-471), on
  `onCurrentFrameMetadataChanged` (475-487), and on `dropCachedOutputMetadata` (452). It is capped at
  `kMaxCachedMetadataFrames = 256` (NativeEffectBase.cpp:356); at the cap it is emptied (421-422).
  A generation check guards the store (420).
- Effective data kind: `invalidateEffectiveOutputDataKindCache` (Node.cpp:2377) clears the memo on
  the node and walks both outputs and inputs, crossing group and precomp boundaries. Called from
  `onInputChanged` (NodeInputs.cpp:1625), for group inputs (NodeInputs.cpp:1696), on a detached
  input (NodeInputs.cpp:69) and from `NativeRead::refreshOutputDataKind` (NativeRead.cpp:468). Only
  polymorphic-declared nodes memoize (Node.cpp:2024). A cycle answer is not memoized
  (Node.cpp:2058-2063).

## Not invalidated by

- Results that ignore their hash. The hash is only the key: the action reads live state. 15 native
  `getRegionOfDefinition` overrides take `U64 /*hash*/`: OfxEffectInstance.cpp:2300, Shuffle.cpp:701,
  NativeRead.cpp:1155, ChromaKeyer.cpp:867, NativeWrite.cpp:1871, Crop.cpp:474, USDRender.cpp:251,
  Keyer.cpp:1067, STMap.cpp:434, IDistort.cpp:306, DeepRead.cpp:93, LensDistortion.cpp:873,
  Merge.cpp:902, Dissolve.cpp:210, CopyMetadata.cpp:83. For example, `STMap::getRegionOfDefinition`
  reads `getInput()` (STMap.cpp:440-441), which returns the render-side `inputs` array
  (`Node::getInput` NodeInputs.cpp:236 → `getInputInternal(false, ...)` 259), and
  `uv->getRenderMetadata` (STMap.cpp:450). The answer reflects the graph when it runs, not the
  graph the hash names.
- Callers pass the live hash instead of the render hash: `makeInfoForInput` (Node.cpp:3026, 3032),
  Node.cpp:6537, KnobTypes.cpp:462, AppInstance.cpp:2213, DiskCacheNode.cpp:188, DeepCrop.cpp:171
  and DeepRecolor.cpp:414 (keys.md K2).
- Same-hash metadata changes drop only metadata and components downstream
  (`onMetadataChangedUnderSameHash` Node.cpp:1809, `dropCachedMetadata` EffectInstance.cpp:7466).
  Downstream RoD, identity and frames-needed states stay. The two callers today also move the hash
  (file stamps through `KnobFile::invalidateFileStamp` → `invalidateContentHash`, KnobFile.cpp:164-171),
  so this is a latent gap rather than a live one.
- Mipmap level for components needed (key forced to mipmap 0, EffectInstancePrivate.cpp:234, 253).

## Metadata during a render

- `getMetadata` (outside `getRenderMetadata`) keys on the live node hash (EffectInstance.cpp:7264).
  A render thread calling it can see a newer graph than its frame args.
- `getRenderMetadata(frameArgs, ...)` (7363) asks `getMetadata`. If the answer's hash differs from
  `frameArgs->nodeHash` and is not the unhashed same-state case (7389), it aborts the frame when the
  render is abortable (7398-7401) and returns the new-state answer anyway (7403). Sequence renders,
  Writes included, are abortable (OutputSchedulerThread.cpp:2436, :2665; abort at :2598-2600), so
  a write never keeps metadata newer than its frame. The non-abortable renders are viewer and
  single-frame requests that write no file.
- `findRenderMetadata` (7334) returns the frame's snapshot or retained answer first, so the mismatch
  path only runs on a snapshot miss.

## Covered by tests

- Seeded stress (`Tests/CacheStress.h`): every reference clears `clearActionsCache` on every node, so a stale action result shows up as a pixel mismatch; `CacheStressTest.MergeSwitchAndDissolveWithGrowingInputs` grows and shrinks RoDs through Switch and Dissolve.
- Metadata: `Tests/PerFrameMetadata_Test.cpp` (16 cases; caching at :272, LRU at :294, concurrent
  stores at :385, hash ownership at :510, purge race at :539, unsettled snapshot abort at :569),
  `Tests/RenderContextMetadata_Test.cpp:140`, `Tests/NativeMetadata_Test.cpp:115,144,166`.
- Context sizing: `Tests/RenderContextRequest_Test.cpp:476` (`ActionsCacheKeepsEveryContextOfTheFrame`).
- Clearing: `Tests/Native/ClearAllCaches_Test.cpp:55`.
- Data kind: `Tests/DataKindGroup_Test.cpp` (13 cases, including the swap cases at :468, :500, :666, :692).
- Identity across scales: `CacheStressTest.IdentityAcrossScales` asks at mipmap 1, then renders at
  mipmap 0.
- No test covers RoD, frames-needed or time-domain cache staleness.

## M202 and M71

- M202 gates the identity, RoD (both stores), frames-needed, frame-range, pass-through,
  components, metadata and time-invariant stores with `CleanHashSnapshot::allowsStoreUnder`. This
  closes the stale-store path for the unchecked results (A2 below). It makes the native metadata
  cache a per-context LRU of 8 contexts behind the same gate (A3).
- M71 changes `getRenderHash` outside a render to mix in the `ActionContextScope` context key (M71
  EffectInstance.cpp:554-570). It also wraps input RoD and identity calls in
  `ActionContextScope(getInputActionContext(...))` (M71 EffectInstance.cpp:525). That moves the
  ActionsCache state a GUI-thread query lands in.

## Suspected holes

- **A2 Unchecked stores.** RoD, identity, frames-needed, time-domain and components stores have no
  generation check or snapshot on this branch, and the live-state readers above can store a newer
  graph's answer under an old hash. **Fixed by M202.**
- **A3 Native metadata cache ignores context.** `_metadataCache` is keyed on (time, view) under one
  node hash (NativeEffectBase.h:635-643, NativeEffectBase.cpp:394). With contexts in use, two
  contexts at the same hash share answers. **Fixed by M202** (per-context LRU).
- **A4 Newer metadata in non-abortable renders.** See "Metadata during a render" (EffectInstance.cpp:7398-7403).
  Open. M260 planned to fix "the frame on disk", but writes are abortable, so that case does not
  arise; what is left is a non-abortable viewer or single-frame request returning new-state
  metadata, which needs a decision on whether those should abort or keep a snapshot.
- **A5 Same-hash drops skip geometry results. Fixed in M260.** A same-hash metadata change now
  clears the node's whole actions cache (identity, RoD and the rest), not only metadata. Test:
  `RawKindFrameworkTest.AStageChangeUnderTheSameHashRecomputesTheCachedIdentity`.

## Fixed (historical)

The analysis below describes the code before the fix.

- **A1 Identity ignored scale.** The identity key had no mipmap level (forced to 0 like components
  needed), but native identity depends on scale. ErodeDilate is identity when
  `floor(size * scale) == 0` (ErodeDilate.cpp:300-306, 413-417). Blur uses pixel sizes at the
  context scale (Blur.cpp:114, 377-381). StagedImageEffect passes the scale through
  (StagedImageEffect.cpp:797). Cached callers use different scales: the render path
  (EffectInstanceRenderRoI.cpp:544, `renderMappedScale`), the frame graph
  (ParallelRenderArgs.cpp:903, `mappedScale`) and Node.cpp:6861 (identity scale). An ErodeDilate of
  size 1 asked at mipmap 1 first cached "identity", and a later full-scale render then skipped the
  node. **Fixed in M242:** `getIdentityResult`/`setIdentityResult` take the mipmap level and key on
  (time, view, mipmap). Stress case: `CacheStressTest.IdentityAcrossScales` (mipmap 1, then 0);
  `eOpMipmap` runs only there.
