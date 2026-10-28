# Viewer cache, on-screen images and the timeline cached-frames line

State recorded on branch `milestone/m242-cache-invalidation-audit`. "Fixed by M202" means PR #131
(`origin/milestone/m202-clean-hash-cache-stores`) closes the entry. "M71" means PR #122
(`origin/milestone/m71-rfv-render-context`). M178 - Render Context Core's timeline fix
(`natronCachedFramesInRAM`, `Cache::visitEntryKeys` reporting the tile cache's disk portion) is
already on this branch.

## Caches

| Cache | Owner | Lifetime |
| --- | --- | --- |
| ViewerCache (texture tiles) | `Cache<FrameEntry>` created AppManager.cpp:1043; its disk portion is restored at startup (`restoreCaches` AppManagerPrivate.cpp:458-462) | LRU; across sessions for the disk portion |
| Roto-paint buffer reuse | `_imp->lastRenderParams[textureIndex]` ViewerInstance.cpp:2175-2227 | Until a non-paint render (:2112, :2241) or `clearLastRenderedImage` (ViewerInstance.cpp:306-311) |
| Device cache fills | `assignReadbackToken`/`addCacheFill` ViewerInstance.cpp:3478-3502, stored at :3370-3381 | Until the readback completes; capped by `kMaxPendingCacheFills` (a dropped fill only loses entries) |
| On-screen images per mipmap level | `TextureInfo::lastRenderedTiles`, `lastRenderedDeepTiles`, `lastRenderedDeviceTiles`, `pendingDeviceRoot` Gui/ViewerGLPrivate.h:148-165 | Until replaced per level or cleared |
| Timeline cached-frames line | `TimeLineGui` `_imp->cachedFrames`, `cachedFramesHash`, `cachedFramesMipmapLevel` (Gui/TimeLineGui.cpp:1436-1460) | Until the next refresh |

`ViewerHandoff` (Engine/ViewerHandoff.{h,cpp}) is a ring of display buffers plus readback tokens
(ViewerHandoff.cpp:881, :895). It keys no content, so it is not a cache here.

## Key

### FrameKey (one row per field)

`FrameKey` fields are at Engine/FrameKey.h:125-140, the constructor at :41-58. `fillHash` is
FrameKey.cpp:90-119 and `operator==` FrameKey.cpp:124. The lookup key is built at
ViewerInstance.cpp:1367-1382, the CPU store key at :2261-2279 and the device store key at
:2081-2100.

| Field | Value at lookup (ViewerInstance.cpp:1367-1382) | Hashed | Compared |
| --- | --- | --- | --- |
| `_time` | `params->time` | yes (:92) | yes |
| `_treeVersion` | `viewerHash`, the viewer's content hash, which folds both active inputs by slot (keys.md step 5) | yes (:93) | yes |
| `_gain` | `params->gain` | only when `!_useShaders` (:94-98) | skipped when both keys use shaders |
| `_gamma` | `params->gamma` | only when `!_useShaders` | as gain |
| `_displayTransformHash` | `displayProcessor->cacheHash`, else 0 | only when `!_useShaders` | as gain |
| `_bitDepth` | `params->depth` | yes | yes |
| `_channels` | `outArgs->channels` (display channels) | yes | yes |
| `_view` | `params->view` | yes | yes |
| `_textureRect` | the tile rect, with `closestPo2` | yes | yes |
| `_mipmapLevel` | `mipmapLevel` | yes | yes |
| `_inputName` | `inputToRenderName` | yes (:117) | yes |
| `_layer` | `params->layer`, ID and channel names | yes (:108-112) | yes |
| `_alphaChannelFullName` | alpha layer ID + alpha channel name | only if non-empty | yes |
| `_useShaders` | `depth == eImageBitDepthFloat` | no | only through the gain clause |
| `_draftMode` | `isDraftMode` | yes (:118) | yes |
| holder | `getNode().get()` (`getCacheHolderID`) | no | used by `getViewerCachedFrames` |

`_useShaders` is a function of `_bitDepth`, which is hashed, so leaving it out is consistent.
With shaders the stored texture is pre-display and the shader applies gain, gamma and the display
transform, so skipping them in that mode is correct.

### When the viewer hash is captured

- Playback: `ViewerRenderFrameRunnable::renderFrame` reads `viewer->getHash()` once per frame
  (OutputSchedulerThread.cpp:3086).
- Current frame: `ViewerCurrentFrameRequestScheduler::renderCurrentFrame` reads it once per request
  (OutputSchedulerThread.cpp:4135).
- `renderViewer_internal` (ViewerInstance.cpp:1654) builds the `ViewerParallelRenderArgsSetter`
  at :1668-1681. That is after `viewerHash` was captured, so frame args come from a later graph
  state than the key.
- `setupMinimalUpdateViewerParams` captures `activeInputHash` from metadata (ViewerInstance.cpp:1189-1191).
  The redraw check at :1756 compares it with the input's current hash, but only when
  `!isSequentialRender`. Playback has no such check.

### When the cache is used

- Lookup: `getViewerRoIAndTexture` (ViewerInstance.cpp:1269) calls `appPTR->getTexture` (:1384).
  With `forceRender`, cached entries are removed first (:1394-1397).
- `getRoDAndLookupCache` (:1445) uses the cache only without a user RoI, auto-contrast, roto paint
  or a partial update (:1452). The render side has the same condition at :1727. Auto-contrast is
  the only source of a non-zero offset (:2353, :2416), and it bypasses the cache.
- CPU stores: `getTextureOrCreate` (:2261-2279, AppManager.cpp:2609) after the upstream image is
  computed (abort checks :2035, :2047), and `setInternalImage` (:2311).
- Device stores: `deviceCacheTiles` keys (:2081-2100), filled when the readback lands (:3370-3381).

## Invalidated by

- Any change to a hashed field gives a new key. Old entries stay, so reverting hits them again.
- `forceRender` removes the looked-up entries (ViewerInstance.cpp:1394-1397).
- `AppManager::clearViewerCache` (AppManager.cpp:1327) and `clearDiskCache` (:1336-1345, which also
  clears the last-rendered textures). `removeFromViewerCache` (:2471, :2489).
- LRU eviction. `Cache::notifyEntryDestroyed` emits `removedEntry` (Cache.h:1344-1363), and the
  emission runs on the deleter thread.

### On-screen images

- `endTransferBufferFromRAMToGPU` replaces `lastRenderedTiles[mipmapLevel]` for the rendered level
  only (Gui/ViewerGL.cpp:1581). The device path resets that level's CPU image (:1593).
- A texture-only frame from the viewer cache resets every level (ViewerGL.cpp:1598-1610).
- `clearLastRenderedImage` (ViewerGL.cpp:2035) and `clearLastRenderedTexture` (:4509). Callers:
  Node.cpp:4467 (`Node::deactivate`), AppInstance.cpp:2408, AppManager.cpp:1323 and :1343,
  NodeGui.cpp:2481, GuiAppInstance.cpp:1144, RotoPaint.cpp:991, RotoContext.cpp:463.

### Timeline line

- `connectSlotsToViewerCache` (TimeLineGui.cpp:1318-1330). `onCachedFrameAdded`, `onCachedFrameRemoved`
  and `onCachedFrameStorageChanged` (:1370-1390) schedule a refresh on a 100 ms timer that is not
  restarted while running (:250-256).
- `onMemoryCacheCleared` (:1393) and `onDiskCacheCleared` (:1410) drop the cleared storage at once
  and schedule a refresh. `clearCachedFrames` (:1426) runs on a zoom-level change (ViewerGL.cpp:3201,
  :3238, :3302, through ViewerTab40.cpp:327-342).
- `refreshCachedFrames` (:1436) lists frames under `node->getHashValue()` (the live content hash,
  Node.cpp:798-801) at `getMipmapLevelWithoutDraft()`, through `AppManager::getViewerCachedFrames`
  (AppManager.cpp:2642-2662). `publishCachedFrames` sets `natronCachedFrames` and
  `natronCachedFramesInRAM` (:259-271).
- `refreshCachedFramesIfStale` (:1464-1473) re-lists when the hash or level moved. It runs only
  when a texture is uploaded (ViewerGL.cpp:1633, :1914).

### The line in the four cases the milestone asks about

- **Edit:** the hash moves. The line keeps the old hash's frames until the next upload or cache
  signal, then lists the new hash's frames (V3).
- **Cache clear:** the cleared storage drops at once, then a refresh re-lists (TimeLineGui.cpp:1393-1423).
- **LRU eviction:** `removedEntry` schedules a refresh. A frame stays listed while any one of its
  tiles is cached (V2).
- **Edit during playback:** `abortViewerRendersForEdit` (EffectInstance.cpp:6239) aborts viewer
  renders and restarts them (`abortRenderingAutoRestart` OutputSchedulerThread.cpp:3487).
  `stopRender` clears the playback buffer (:1260-1263). Frames started after the edit read the new
  hash. A frame in flight keeps the hash it captured (V1). The line follows on the next upload.

## Not invalidated by

- An edit that changes nothing in `FrameKey`, by design.
- Gain, gamma and the display transform in float (shader) mode, by design.
- The roto-paint reuse checks only the mipmap level and rect containment
  (ViewerInstance.cpp:2175-2227). It is cleared by any non-paint render, so it does not outlive a
  stroke.
- The disk portion across sessions (`restoreCaches`). Keys carry the viewer hash, which includes the
  binary stamp (keys.md), so restored entries hit only under an identical graph.

## Covered by tests

- Seeded stress (`Tests/CacheStress.h`): `CacheStressTest.ViewerABProbe` renders the viewer's B input through the caches and checks that one viewer key (shown input, viewer hash, time, mipmap) never answers two different images. `Tests/gui/cache_invalidation_viewer.py` disconnects an STMap's Map while idle, during a scrub and during playback, and reads back the viewer and the cached-frames line.
- `Tests/ViewerDisplayTransform_Test.cpp:340` (`DifferentViewsGiveDifferentFrameKeys`), `:403`
  (`FrameKeyHashDependsOnTransformWithoutShaders`), `:415` (`FrameKeyHashIgnoresTransformWithShaders`).
- `Tests/HashInvariants_Test.cpp:773` (`ViewerCachedFramesListTheCurrentHashAndLevelInRam`).
- `Tests/gui/m178_timeline_cache_line.py` (edit, undo, clear) and `Tests/gui/m178_cache_line.py`
  (playback, Blur edit, undo, clear).
- No test covers a mid-playback edit, LRU eviction of part of a frame, or the on-screen level
  fallback.

## M202 and M71

- M202 makes `getRenderViewerArgsAndCheckCache` take a `CleanHashSnapshot` (`outArgs->cleanHash`).
  It gates `deviceCacheTiles` with `cleanHash->allowsStoreUnder(viewerHash)`, and it removes the
  CPU tiles a render created from the viewer cache when the snapshot no longer allows the store.
  That closes V1.
- M71 changes no viewer key. The viewer hash is the content hash, and context keys enter only the
  render hash (keys.md).

## Suspected holes

- **V1. Key captured before the frame args.** `viewerHash` is read at OutputSchedulerThread.cpp:3086
  or :4135, before `ViewerParallelRenderArgsSetter` (ViewerInstance.cpp:1668-1681). Playback
  skips the hash-changed check (:1756). An edit landing in between stores tiles rendered from the
  newer graph under the older hash, and undoing the edit then serves them. **Fixed by M202.**
- **V2. The line over-reports.** `getViewerCachedFrames` filters only on tree version, mipmap level
  and holder (AppManager.cpp:2651-2653). It marks a frame when any one tile exists, whatever its
  view, channels, layer, input name, draft flag, or how much of the frame the tiles cover. A frame
  cached only in another view, layer or channel mode, or only partly, is drawn as cached. Display
  only: no pixels are wrong.
- **V3. The line refreshes only on uploads and cache signals.** After an edit whose render fails,
  draws black, or is paused, nothing uploads and no cache signal fires. The line keeps the old
  hash's frames until the next event (TimeLineGui.cpp:1464-1473, ViewerGL.cpp:1633, :1914). Display
  only.
- **V4. Other levels keep earlier images.** A new image replaces only its own level
  (ViewerGL.cpp:1581). `getLastRenderedImage` returns the first level that holds an image, starting
  at level 0 (ViewerGL.cpp:4583-4606). `getLastRenderedImageByMipmapLevel` falls back to other
  levels (:4640-4656). After an edit rendered at level 1, a level-0 image of the pre-edit graph
  can be returned to their callers (picker, histogram). **Fixed in M242:** a new image or device
  root clears the other levels, as the texture-only path already did. No automated test: the
  stored levels are only reachable through the GUI.
- **V5. A/B switch and the line.** Changing the A/B selection does not mark the viewer hash
  (topology.md T2). `_inputName` keeps the texture lookup right, but the line filters by hash
  only, so it keeps listing the old input's frames. **Fixed in M242** for the hash: the switch
  now recomputes it (topology.md T2). That the line redraws on the switch is not checked in the
  GUI. Stress case: `CacheStressTest.ViewerABSwitch`.
