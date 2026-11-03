# GPU caches

State recorded on branch `milestone/m242-cache-invalidation-audit`. "Fixed by M202" means PR #131
(`origin/milestone/m202-clean-hash-cache-stores`) closes the entry. "M71" means PR #122
(`origin/milestone/m71-rfv-render-context`). Paths are relative to `Engine/` unless stated.

## Caches

| Cache | Owner | Lifetime |
| --- | --- | --- |
| `GpuTextureCache` | Gpu/GpuTexture.h:91-128; one per runtime, `GpuRuntime::textures_` (Gpu/GpuRuntime.cpp:467, parked :496, reset :504, handed to the record context :598) | LRU over unheld entries, 256 MiB budget (`kDefaultBudgetBytes` Gpu/GpuTexture.h:93); runtime lifetime |
| `GpuPipelineCache` | Gpu/GpuPipelineCache.h:39-92 | In memory for the runtime; on disk across sessions |
| Host import cache | `std::map<uintptr_t, CachedImport> importCache` Gpu/GpuTransfer.cpp:347 | Until `forget` (Gpu/GpuTransfer.cpp:967-1000) or runtime teardown |
| FrameStore device residency | `FrameStore::putDevice` (GpuRegionTask.cpp:2970) | Per frame render; evicted to RAM under device pressure (:2975-2983, :3002) |
| `StripMemory` | GpuRegionTask.cpp:3304-3354 (process static) | Process; `forget()` on device loss (:3423) |
| `GpuBufferPool` | Gpu/GpuBufferPool.h:86 | Runtime; pooled device buffers by size class |
| `StageGpuShaderGate` | Nodes/Image/StagePlan.h:415-427, member `_gpuShaderGate` of `StagedImageEffect` | Node; mapped in [node-private.md](node-private.md) |
| `CurveTablesCache` | Nodes/Image/CurveSnapshot.h:384-424 | Node; mapped in [node-private.md](node-private.md) |

## Key

| Cache | Key | Identifies the description | Identifies the texels |
| --- | --- | --- | --- |
| `GpuTextureCache` | caller-supplied string. `acquire(key, desc, texels, ...)` (Gpu/GpuTexture.h:101) returns the held texture for a known key whatever `desc` and `texels` are passed | only if the caller puts it in the key | only if the caller puts it in the key |
| ... OCIO LUTs (the only producer) | `textureKey` (OcioVulkanShader.cpp:95-110): `"ocio-lut:%016llx"` over every `GpuTextureDesc` field (dimensions, width, height, depth, channels, filter; struct Gpu/GpuTexture.h:20-30) and the texel bytes | yes | yes (64-bit hash) |
| `GpuPipelineCache` | `makeKey` (Gpu/GpuPipelineCache.cpp:176-196): file version, glslang version, `deviceTag_` (vendor:device:driver, :156), stage, entry point and the full source text; disk entries are checked by checksum | n/a | n/a (the source is the content) |
| Host import cache | host address (`uintptr_t`) of a range aligned to `importAlignment` in base and length (Gpu/GpuTransfer.cpp:807, alignment from `minImportedHostPointerAlignment` :473) | address and length | no (G1) |
| FrameStore device residency | `TaskKey` (node, time, view, mipmap, context key); see [image-caches.md](image-caches.md) | yes | yes, within one frame render |
| `StripMemory` | (generation, shape key) | n/a | holds tuning only, no content |
| `GpuBufferPool` | size class | n/a | no content is assumed on lease |

Users of `GpuTextureCache`: `GpuRecordContext::cachedTexture` (Gpu/GpuRecord.cpp:163-180), reached
from `StagePlan.cpp:595` only when the stage texture's `cacheKey` is non-empty. The only non-empty
key is the OCIO one, set at OcioVulkanShader.cpp:164 and :183, acquired at :288 and passed through
Nodes/Color/OcioNodeBase.cpp:196. Every other stage texture goes through `ctx.texture` uncached.

Pipeline callers: Nodes/Image/StagePlan.cpp:1654, ViewerGpuProcess.cpp:301, Gpu/GpuRecord.cpp:115.

### Curve tables

The tables uploaded for ColorCorrect, HueCorrect and ColorLookup come from `CurveTablesCache`
(Nodes/Image/CurveSnapshot.h:384-424), keyed by `knob.getContentHash()` and stored only when the
hash is unchanged after the build. They are uploaded through the uncached `ctx.texture` path, so
the GPU holds no separate copy keyed differently. Their staleness is the knob content hash's: they
inherit E1 ([expressions-and-links.md](expressions-and-links.md)).

## Invalidated by

- `GpuTextureCache`: a different OCIO processor or LUT changes the key. Unheld entries are trimmed
  by LRU. The cache dies with the runtime (Gpu/GpuRuntime.cpp:504).
- `GpuPipelineCache`: any source, entry, stage, compiler or driver change gives a new key.
- Host import cache: `GpuTransfer::forgetHostAllocation` (Gpu/GpuTransfer.cpp:1690) calls `forget`.
  It is called only from the `HostImageMemory` free hook (GpuContext.cpp:472-477), which runs in
  `reallyFree` (HostImageMemory.cpp:270-288). The contract is stated at Gpu/GpuTransfer.h:24-27.
  `cacheHostImports` is turned on at GpuContext.cpp:481.
- Region-resident device images: outputs are put in FrameStore under `task.key`
  (GpuRegionTask.cpp:2966-2970). Roots and members are made evictable (`setEvictable`,
  GpuContext.cpp:828; GpuRegionTask.cpp:2975-2983, :3002), so device pressure moves them to RAM
  (`evictToRam`). They are dropped with the FrameStore at the end of the frame render.
- `StripMemory::forget` on device loss (GpuRegionTask.cpp:3423).

## Not invalidated by

- `GpuTextureCache` key reuse with different content. The cache trusts the key
  (Gpu/GpuTexture.h:101). Safe today because the only producer hashes the description and texels.
  A future producer with a name-only key would serve stale LUTs.
- Host imports of memory that `HostImageMemory` does not free (G1).

### Can a GPU result enter a cache under a key the CPU would compute differently?

- **Image cache: no.** GpuRegionTask.h:48-56 says nothing goes to the image cache, and the only
  stores are `FrameStore::put`/`putDevice` (GpuRegionTask.cpp:189, :2967, :2970).
- **FrameStore: same key.** `TaskKey` is built by the scheduler for both placements. The shared
  output key (RenderScheduler.cpp:1476-1481) is (node, node hash, view, cache mipmap level,
  `cacheKeyHasTime`, time), with `cacheKeyHasTime` from `isFrameVaryingOrAnimated`
  (RenderScheduler.cpp:609).
- **Placement probe.** `GpuPlacement.cpp:891-912` `isCached` tries both `fullScale` flags with pixel
  aspect 1 and non-draft. The CPU key carries draft (EffectInstanceRenderRoI.cpp:817-832). It only
  looks up, and a false result only keeps a task on the CPU (:891).
- **Viewer cache:** device tiles are stored under the CPU `FrameKey` (ViewerInstance.cpp:2081-2100);
  see [viewer-and-timeline.md](viewer-and-timeline.md). M202 gates that store.

## Covered by tests

- Seeded stress (`Tests/CacheStress.h`): `CacheStressGpuTest` (`Tests/Gpu/CacheStressGpu_Test.cpp`) renders each seeded step with every accepted task placed on the GPU, through the caches, against a CPU reference from empty caches within 1e-4; `CurveEditsReachTheGpuFrame` moves ColorLookup control points and back, and `LutSwapsReachTheGpuFrame` swaps an OCIOFileTransform LUT and back, which goes through `GpuTextureCache`. Without a device it prints `[  SKIPPED ]` (NATRON_TEST_REQUIRE_GPU=1 turns that into a failure).
- `Tests/Gpu/GpuPipelineCache_Test.cpp:85-353` (10 cases: key fields, disk round trip, checksum).
- `Tests/Gpu/GpuTransfer_Test.cpp:707` (`ForgottenHostAllocationIsReimported`), `:784`, `:849`, `:907`.
- `Tests/FrameRenderContext_Test.cpp:420` (FrameStore device residency).
- No test checks `GpuTextureCache` keying; `OcioVulkanShader_Test.cpp` checks the textures only.
- No test imports a non-`HostImageMemory` buffer and then reuses its address.

## M202 and M71

- M202 gates the viewer's device cache stores (`allowsStoreUnder`); no other GPU cache changes.
- M71 adds the context key to `TaskKey`; the GPU uses the same key.

## Suspected holes

- **G1 Address-keyed import outliving a buffer it did not see freed.** `resolveImports`
  (Gpu/GpuTransfer.cpp:800-900) caches any aligned range, not only `HostImageMemory`.
  `importSources` (GpuRegionTask.cpp:1405-1427) limits explicit imports to `HostImageMemory`
  (`bytesFrom` :1418), but `_runtime.upload` at GpuRegionTask.cpp:1153 and :2258 passes any source
  image's pixels, and the upload goes through the same cache. Only GL-texture images are excluded
  (:1085). Two sources skip the free hook: `HostImageMemory::allocate` uses plain `std::malloc` below
  `kMinPooledBytes` (1 MiB, HostImageMemory.cpp:57, :484-491) and frees it without the hook
  (:589-591, :630-632); disk-cache images are `mmap`ed (`MemoryFile`, CacheEntry.h:428, :555).
  A page-aligned, page-multiple range from either can be unmapped or freed while its import stays
  cached. A later allocation at the same address with the same length then uploads through the
  stale import. **Fixed in M260:** only transfers that set `cacheImport` (memory whose free calls
  `forgetHostAllocation`) keep a new import; other ranges are imported per transfer. Test:
  `CacheStressGpuTest.DiskCacheSourcesAreNotImportedByAddress`.
- **G2 Shader gate ignores the device.** `StageGpuShaderGate::accept` keys `_rejected` by entry and
  source only (Nodes/Image/StagePlan.cpp:1639-1644, :1681). A shader rejected on one device stays
  rejected after a device switch. Placement only: the node runs on the CPU. Listed in
  [node-private.md](node-private.md). **Fixed in M260:** rejections are keyed by the pipeline
  cache's device tag (vendor, device, driver) as well, so a device switch retries the shader. Test:
  `CacheStressGpuTest.DeviceSwitchRetriesRejectedShader`.
