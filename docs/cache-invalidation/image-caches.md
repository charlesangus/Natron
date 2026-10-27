# Image caches

State recorded on branch `milestone/m242-cache-invalidation-audit`. "Fixed by M202" means PR #131
(`origin/milestone/m202-clean-hash-cache-stores`) closes the entry. "M71" means PR #122
(`origin/milestone/m71-rfv-render-context`).

## Caches

| Cache | Owner | Entry key | Lifetime |
| --- | --- | --- | --- |
| NodeCache | `Cache<Image>` "NodeCache", AppManager.cpp:1041 | `ImageKey` | RAM, LRU, app-wide |
| DiskCache | `Cache<Image>` "DiskCache", AppManager.cpp:1042 | `ImageKey` | Disk, LRU, restored across sessions (AppManagerPrivate.cpp:461) |
| DeepImageCache | `Cache<DeepImageCacheEntry>` "DeepImageCache", AppManager.cpp:1046 | `DeepImageKey` | RAM, LRU, app-wide |
| FrameStore | `FrameRenderContext::_store` FrameRenderContext.h:355, `class FrameStore` :58 | `FrameStore::TaskKey` | One frame render; an entry lives until its consumers release it |
| DiskCache node | `DiskCacheNode`, storage forced to disk at EffectInstanceRenderRoI.cpp:715-716 | `ImageKey` in DiskCache | As DiskCache |

ViewerCache (AppManager.cpp:1043) is in the Phase 2 viewer document.

The cache looks up an entry by the key's hash bucket (`Cache::getInternal` Cache.h:1735, buckets
at :1742 and :1763), then by `operator==` (Cache.h:1749, 1775). `KeyHelper` records the holder's
cache ID (KeyHelper.h:75), but neither `ImageKey::fillHash` nor `operator==` uses it. Entries are
therefore shared by content: two nodes with equal hashes share images.

## Key

### ImageKey (ImageKey.h:42-51; ImageKey.cpp:48-98)

| Field | In hash (ImageKey.cpp:69-78) | In `==` (:82-98) | Set by renderRoI (EffectInstanceRenderRoI.cpp:817-832) | Omits |
| --- | --- | --- | --- | --- |
| `_nodeHashKey` | yes (:71) | yes | `frameArgs->nodeHash` (:379), the per-frame captured hash (keys.md) | everything the content hash omits (keys.md K3-K8) |
| `_time` | only if `_frameVaryingOrAnimated` (:72-73) | only if the left operand is frame-varying (:84) | `args.time` | time, for a node that is not frame-varying or animated |
| `_pixelAspect` | yes (:76) | yes | always `1.` (:822, :830) | the real pixel aspect |
| `_view` | yes (:75) | yes | `args.view` | — |
| `_draftMode` | yes (:77) | yes | `draftModeSupported && frameArgs->draftMode` (:823); a second key with `false` is looked up first (:825-832, :882-888) | — |
| `_frameVaryingOrAnimated` | no (it selects whether time is hashed) | selects the comparison | `frameArgs->isFrameVaryingOrAnimated`, else `isFrameVaryingOrAnimated_Recursive()` (:801) | see below |
| `_fullScaleWithDownscaleInputs` | yes (:78) | yes | `renderMappedMipmapLevel == 0 && !renderScaleOneUpstreamIfRenderScaleSupportDisabled` (:824, :832) | — |
| holder ID (`KeyHelper`) | no | no | the node (`getNode().get()`, :817) | used only for holder removal |

Not in the key at all; checked per entry after lookup in `getImageFromCacheAndConvertIfNeeded`
(EffectInstance.cpp:2053):

- Mipmap level. An exact level is a hit. A lower level in RAM is downscaled (EffectInstance.cpp:2132-2166).
  The constructor notes that every level shares the key (ImageKey.cpp:52).
- Layer. Colour layers count as interchangeable (`convertible`, EffectInstance.cpp:2131).
- Bit depth. Equal or deeper storage matches (EffectInstance.cpp:2133).
- Project-format RoD. An entry whose RoD followed the project format is dropped when the format
  changed (EffectInstance.cpp:2109-2117).
- Disk entries are found through `getImage_diskCache` (AppManager.cpp:2567).

### DeepImageKey (DeepImageKey.h:46-49; DeepImageKey.cpp:53-64)

| Field | In hash and `==` | Set at | Omits |
| --- | --- | --- | --- |
| `_nodeHashKey` | yes | `frameArgs->nodeHash` (EffectInstanceRenderDeep.cpp:267, key at :351, :702, :795) | as ImageKey |
| `_time` | always | `args.time` | — |
| `_view` | yes | `args.view` | — |
| `_mipmapLevel` | yes | from `args.scale` | — |
| layers, draft | no | — | channels live in the entry; draft is not distinguished |

Lookup skips entries that are not fully rendered and needs `bounds ⊇ roi`
(EffectInstanceRenderDeep.cpp:136-160, 363-372). A writer rendering its own sequence never looks up
(:360).

### FrameStore::TaskKey (FrameRenderContext.h:60-69; FrameRenderContext.cpp:40-57)

| Field | In `==` (:42) | In hash (:46-57) | Set by consumer lookup (`lookupFrameStore` EffectInstance.cpp:942) |
| --- | --- | --- | --- |
| `node` | yes (pointer) | yes (:48) | `input->getNode()` (:956) |
| `time` | yes | yes, `-0.` folded to `0.` (:50-52) | `time` (:957) |
| `view` | yes | yes (:53) | `view` (:958) |
| `mipmapLevel` | yes | yes (:54) | `mipmap` (:959) |
| `contextKey` | yes | only if non-zero (:55-56) | `inputContext->contextKey()` or 0 (:960) |

No node hash and no draft flag: the store lives inside one `FrameRenderContext`, whose frame args
fix both. Layers are the entry's map (`Entry::layers`, FrameRenderContext.h) and are matched in
`find` against the needed list (FrameRenderContext.h:139). Task keys for the scheduler graph are
built in RenderScheduler.cpp:510-546.

### Frame-varying test (`isFrameVaryingOrAnimated_impl` EffectInstance.cpp:6361-6384)

A node is frame-varying if any of these holds, or any input's does (6371-6379):

1. `getTimeInvariantMetadata()->getIsFrameVarying()` (6370). The default metadata sets it from
   `Node::hasAnimatedKnob()` (EffectInstance.cpp:6684), which asks `KnobHelper::isAnimated` →
   `curve->isAnimated()` (Knob.cpp:1952-1961): keyframes only. Nodes that set it themselves:
   NativeRead.cpp:1072, USDRender.cpp:240, DeepRead.cpp:184, Posterize.cpp:457, and OFX through
   `kOfxImageEffectFrameVarying` (OfxImageEffectInstance.cpp:1339).
2. `KnobHolder::getHasAnimation()`. It walks the holder's knobs on every call: a dimension
   varies with time if it has an expression, keyframes, or a master (followed recursively) that
   does. There is no stored flag, so the answer does not depend on edit history.
3. `getNode()->getCurveContext()` (roto).

The answer is computed once per frame per node (ParallelRenderArgs.cpp:2012-2013) and walks the
render-side inputs (`getInput`).

**Does an expression on `frame` with no keyframes make a node frame-varying? Yes**, through
test 2. Any expression counts, whether or not it reads the time: an expression's dependencies are
not known well enough to rule time out. See hole I1 for what it was before.

## Invalidated by

- Hash change. A new hash is a new key. Old entries are not removed; they are evicted by LRU, so
  an undo back to an old hash hits them. Nothing calls a remove-by-different-hash path:
  `removeAllEntriesWithDifferentNodeHashForHolderPrivate` (Cache.h:1679) is reached only with
  `removeAll = true` from `removeAllEntriesForHolderPublic` (Cache.h:1630-1637) and the cleaner thread
  (Cache.h:332).
- Holder removal. `AppManager::removeAllCacheEntriesForHolder` (AppManager.cpp:2528) clears the
  node's entries in NodeCache, DiskCache, ViewerCache and DeepImageCache by cache ID. Its only caller
  is `Node::removeAllImagesFromCache` (Node.cpp:1901, call 1913), which skips projects that are
  closing or loading (1910). That is called by node destruction (NodeMain.cpp:712), an aborted
  paint-stroke render (EffectInstanceRenderRoI.cpp:1539), precomp reload (PrecompNode.cpp:682) and
  `NativeWrite::purgeCaches` (NativeWrite.cpp:1592, 1609). The cache ID is the node's first script
  name, serialized so disk entries persist (NodePrivate.h:368-372, NodeName.cpp:92, 416-422).
- `AppManager::clearAllCaches` (AppManager.cpp:1380): aborts viewers, drains the deleter threads
  (1392-1404), then `clearDiskCache` (1407; viewer and disk caches, 1337-1345), `clearNodeCache`
  (1408; 1349-1361), `_deepImageCache->clear()` (1409), and per app `clearOpenFXPluginsCaches`
  (1416) and `renderAllViewers(true)` (1420).
- Project format change: project-format entries dropped on lookup (EffectInstance.cpp:2109-2117).
- FrameStore: dies with its `FrameRenderContext`. An entry also goes once its consumer count reaches
  zero (`put` FrameRenderContext.h:126).
- Persistence: `NATRON_CACHE_VERSION` is 10 (GlobalDefines.h:97). On launch the stored version is
  compared (AppManager.cpp:1052-1059). A mismatch or `--clear-cache` wipes the disk structure
  (1062-1064); otherwise `restoreCaches` reloads the ViewerCache and DiskCache tables of contents
  (AppManagerPrivate.cpp:458-462). NodeCache and DeepImageCache are memory-only. A restored entry
  can only hit under an identical node hash. That hash includes the binary content stamp and the
  plug-in stamps (keys.md), so a rebuilt binary orphans every restored entry instead of reusing it.

## Not invalidated by

- Time, for nodes the frame-varying test misses (I1).
- Holder: content sharing means `removeAllImagesFromCache` on one node leaves identical-hash
  entries stored by another node's ID.
- Mipmap level, layer and bit depth are not in `ImageKey`; they are filtered per entry (above).
- Draft for the deep cache (no draft field).

## Covered by tests

- Seeded stress (`Tests/CacheStress.h`): `Tests/CacheStress_Test.cpp` and `Tests/CacheStressSelf_Test.cpp` (the self-test plants a member-cache hole and checks it is caught); seeds replay with `NATRON_CACHE_STRESS_SEED`.
- DeepImageCache: `Tests/DeepImageCache_Test.cpp:120` (round trip), :162 (own budget), :248
  (`clearAllCaches`), :282 (holder purge), :348, :382, :440.
- FrameStore: `Tests/FrameStoreHandoff_Test.cpp:348, 359, 399, 410`.
- `clearAllCaches`: `Tests/Native/ClearAllCaches_Test.cpp:55` (ActionsCache side).
- No test checks `ImageKey` time omission, the frame-varying test, cross-session persistence or
  cache-on versus cache-off equivalence for NodeCache.

## M202 and M71

- M202 adds a `CleanHashSnapshot` in renderRoI that turns off `createInCache` when the captured hash
  is no longer the node's settled hash, and retracts pending regions. The deep path gets the same
  gate on lookup and store. The DiskCache node's input frame range moves from `input->getHash()` to
  `input->getRenderHash()` (DiskCacheNode.cpp:188).
- M71 does not change `ImageKey` or `TaskKey`. Its context keys reach the image caches only through
  the node hash (`frameArgs->nodeHash`, which carries the context key; keys.md).

## Suspected holes

- **I1 Expression on frame/time without keyframes is not frame-varying.** **Fixed in M242.**
  Test 2 used a stored flag. Setting an expression did update it (`expressionChanged` called
  `updateHasAnimation`), but a slave whose master gained keys or an expression never did, and
  link-group and key copies set it from the copied knob only. Such a node dropped time from its
  `ImageKey` and was served the same image at every frame. The flag is now computed live from the
  knobs (see test 2 above). Tests: `CacheStressTest.FrameExpression` (a `frame*0.1` expression,
  then two seeks; `eOpFrameExpression` is now in `defaultOps()`) and
  `Tests/ExpressionFrameVarying_Test.cpp` (expression, slave of an expression, static again once
  the expression and keys are gone).
- **I2 Stores from a render whose graph moved.** The key's hash is captured before render while knobs
  and inputs are read live (keys.md K1, topology.md T1). The entry is then served to the old
  state's hash. **Fixed by M202.**
- **I3 DiskCache persistence across holders.** The cache ID is the first script name and is unique
  only within a project (NodePrivate.h:371). Restored entries from another project with the same
  first name and an equal hash are shared by content, which is correct only if the hash is
  complete. That inherits keys.md K3-K8. Low risk; not fixed by M202.
- **I4 Colour layers interchangeable on lookup.** `convertible` treats any two colour layers as
  matching (EffectInstance.cpp:2131) because the key has no layer. Unverified whether an entry
  stored for RGB can answer an RGBA request with a synthesized alpha. Worth a case in the Phase 3
  stress suites. Not touched by M202.
