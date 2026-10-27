# Node-private caches

State recorded on branch `milestone/m242-cache-invalidation-audit`. "Fixed by M202" means PR #131
(`origin/milestone/m202-clean-hash-cache-stores`) closes the entry. "M71" means PR #122
(`origin/milestone/m71-rfv-render-context`). Paths are relative to `Engine/` unless stated.

A node's own state enters its content hash only through its knobs and the opt-in
`appendNodeContentHash` (EffectInstance.h:403; K5 in [keys.md](keys.md)). Overrides:
Nodes/Scene/USDTransform.cpp:262, Nodes/Scene/USDRead.cpp:273, Nodes/Channel/AddLayers.cpp:375,
Nodes/Deep/DeepAddLayers.cpp:353. File changes on disk reach the hash through `FileStampRegistry`
(FileStampRegistry.h:40-47): a file is stamped (path, mtime, size, inode) on first use and keeps
that stamp until `invalidate`, which Reload does (`KnobFile::invalidateFileStamp`
KnobFile.cpp:164-190).

## Caches

Columns "Edit", "Input" and "Disk" say whether the cache can disagree with the content hash after a
knob edit, an input change, or a file changed on disk.

| Cache | Owner | Key | Edit | Input | Disk |
| --- | --- | --- | --- | --- | --- |
| `SceneLayerMemo` (authored USD layer) | Nodes/Scene/SceneLayerMemo.h:40-110, `produce` Nodes/Scene/SceneOps.cpp:357-440 | `layerHash`: `nodeDigest`, upstream-read mode, the upstream stack hash or `factsDigest`, frame range if read, fps (:365-385) | no: a layer whose key moved during authoring is not memoized (N1, fixed in M260) | no; facts mode is as right as each node's `factsDigest` | via the stamp model (USDRead N2, fixed in M260) |
| `CurveTablesCache` | Nodes/Image/CurveSnapshot.h:384-424; users ColorCorrect.h:161, HueCorrect.h:167, ColorLookup.h:157 | `knob.getContentHash()`, stored only when the hash is unchanged after the build | only through E1 | n/a | n/a |
| `StageGpuShaderGate` | Nodes/Image/StagePlan.h:415-427, member Nodes/Image/StagedImageEffect.h:543 | entry + source (Nodes/Image/StagePlan.cpp:1639-1644) | no | no | n/a; ignores the device (G2) |
| AddLayers registry signature | Nodes/Channel/AddLayers.h:218, Nodes/Deep/DeepAddLayers.h:156 | layer registry signature, folded into `appendNodeContentHash` (AddLayers.cpp:360-379) with `markHashDirty` on change | no | no | n/a |
| NativeRead frame listings | `_listing`, `_proxyListing` Nodes/IO/NativeRead.h:260-262, built NativeRead.cpp:528-591 | canonical pattern, with an invalidation-counter guard | no (cleared on filename/proxy edits :829, :987) | n/a | until Reload (`purgeCaches` :609-650), same as the stamp |
| USDRead file layer and closure | `Implementation` Nodes/Scene/USDRead.cpp:81-100 | `closureDigest` (stamps of every file brought in) in `appendNodeContentHash` (:273-277) | no | n/a | no: stamped when the node is created or loaded and when File changes (N2, fixed in M260) |
| NativeWrite read-back files | `_readBackFiles` Nodes/IO/NativeWrite.h:347-349, NativeWrite.cpp:1425-1470, purge :1564-1590 | output path, validated against the stored output hash (:1430) | no | no | until purge |
| USDTransform matches memo | Nodes/Scene/USDTransform.cpp:142-157 | (matched stack hash, pattern, op name) | no | no | n/a |
| RawDemosaic capture and green memos | Nodes/Raw/RawDemosaic.h:265, RawDemosaic.cpp:980-1004, :1072-1080 | `RawNodeSupport::frameKey(getRenderHash(), time, view)`, stored only if not cancelled | no | no | as the decode cache (N3) |
| IErodeDilate half widths | `static thread_local` Nodes/Filter/IErodeDilate.cpp:585, class :85-110 | (rx, ry); memo of a pure function | no | no | n/a |
| OIIO header cache | Nodes/IO/OiioReadSupport.cpp:63-80, :145-221 | (path, `FileIdentity`) with an eviction counter; `evictHeaders` | no | n/a | pinned to the stamp's identity until Reload (N3, fixed in M260) |
| Raw decode caches | Nodes/IO/RawDecodeSupport.cpp:78-131, :730-760 | `FileKey` (path, `FileIdentity`); `evict` | no | n/a | as OIIO (N3, fixed in M260) |
| DeepRead layers memo | Nodes/Deep/DeepRead.cpp:190-229 | filename; cleared on `purgeCaches` | no | n/a | until Reload, same as the stamp |
| Hydra render engines | Nodes/Scene/HydraRenderImpl.cpp:99, :671-680 | per node; the stage is synced every render | no | no | n/a |
| NativeEffectBase output metadata | Nodes/NativeEffectBase.h:640-643 | see A3 in [actions-and-metadata.md](actions-and-metadata.md) | fixed by M202 | | |
| Roto geometry hash memo | RotoItem.cpp:572-640 | generation | no | n/a | n/a |
| Roto stroke dot patterns | RotoStrokeItem.cpp:489-501, RotoContext.cpp:1040-1061 | not a cache in effect: the store is commented out (RotoContext.cpp:1372), so a pattern is made per dot | | | |

Hydra engines are released by `SceneCaches::releaseRenderEngines` (Nodes/Scene/SceneCaches.h:38-50;
callers AppManager.cpp:1372, :1412, Project.cpp:3252). Their GPU budget is at HydraRenderImpl.cpp:88.

## Key

`SceneLayerMemo` `nodeDigest` (Nodes/NativeEffectBase.cpp:888-911) hashes the plug-in ID and
version, `getKnobsContentDigest`, `appendNodeContentHash`, and the fully qualified name when an
expression exists. It leaves out the input hashes on purpose (:886-887) and the project digest;
the frame range and fps are added separately. Facts users: USDPrim.cpp:214, USDMaterial.cpp:365,
USDPrune.cpp:181, USDLight.cpp:314, USDCamera.cpp:216, USDTransform.cpp:452. USDRead.cpp:367
reads nothing upstream.

## grep audit

Every hit of `grep -rn "mutable\|static thread_local" Engine/Nodes`:

| Hit | Classification |
| --- | --- |
| Nodes/KnobReader.h:110 `_reads` | not a cache: a read counter |
| Nodes/Image/CurveSnapshot.h:421-423 | `CurveTablesCache` (table above) |
| Nodes/MissingPluginNode.h:96 `_storedMutex` | not a cache: guards the stored serialization |
| Nodes/Image/StagedImageEffect.h:543 `_gpuShaderGate` | `StageGpuShaderGate` (table above, G2) |
| Nodes/Channel/AddLayers.h:218, Nodes/Deep/DeepAddLayers.h:156 | registry signature (table above) |
| Nodes/IO/NativeRead.h:260-262 | frame listings (table above) |
| Nodes/IO/NativeWrite.h:347-349 | read-back files (table above) |
| Nodes/Scene/USDRead.cpp:82 `fileLayerMutex` | guards the file layer and closure (table above, N2) |
| Nodes/Scene/HydraHandoff.cpp:101, :158 | not a cache: mutexes for the hand-off state |
| Nodes/Scene/HydraHandoff.cpp:416 | not a cache: a `mutable` lambda |
| Nodes/Scene/USDTransform.cpp:143 | guards the matches memo (table above); `lastUpstream` there drives handles only |
| Nodes/Scene/SceneQueryPool.h:112 | not a content cache: idle stages synced to the payload, whose layers are immutable |
| Nodes/Raw/RawDemosaic.h:265 `_captureMutex` | guards the capture memo (table above) |
| Nodes/Deep/DeepMerge.cpp:165, :210 | not a cache: per-thread scratch written before it is read |
| Nodes/Filter/IErodeDilate.cpp:585 | half-widths memo (table above) |

Hits on the word "immutable" are comments, not caches: KnobReader.h:163, CurveSnapshot.h:48, :384,
OCIO/OcioNodeBase.cpp:94, Filter/BlurKernels.h:80, Metadata/ImageMetadata.h:55, README.md:463,
:859. Not a grep hit but also not a cache: `thread_local readDepth` in
Nodes/Scene/SceneWriteBarrier.cpp:65 (a re-entrancy counter) and RawColorCalibration's `_last*`
members (RawColorCalibration.h:509-511; knob UI state used in `knobChanged`, RawColorCalibration.cpp:930-946).

## Invalidated by

- `invalidateSceneLayer` (NativeEffectBase.cpp:915), called from USDRead.cpp:268 and
  USDTransform.cpp:254 (`onScriptNameChanged`, :250-257). Otherwise the memo is replaced when
  `layerHash` moves.
- `purgeCaches` (Reload) for NativeRead, USDRead, NativeWrite and DeepRead, with the file stamps.
- `markHashDirty` for AddLayers and DeepAddLayers on a registry change; RotoStrokeItem at stroke end
  (RotoStrokeItem.cpp:307-329).

## Not invalidated by

- A file changed on disk without Reload, by design (the stamp model). The OIIO and Raw readers
  then fail on the changed file rather than read it (N3).
- A device switch, for `StageGpuShaderGate` (G2).

## Covered by tests

- Scene layer memo: `Tests/Scene/SceneLayerMemo_Test.cpp:93-171` (reuse, changed key, changed
  upstream, failed authoring, invalidate) and the chain tests at :357-577 (re-authoring only the
  edited layer, facts-reading ops). `SceneLayerMemoTest.ALayerWhoseKeyMovedWhileAuthoringIsNotMemoized`,
  `SceneLayerChainTest.AKnobEditDuringAuthoringIsNotServedAfterUndo` and
  `CacheStressConcurrentTest.SceneKnobEditDuringAuthoring` edit a knob while a layer is being
  authored (N1).
- Reload: `Tests/Native/NativeReadReload_Test.cpp`.
- A file read by OIIO or Raw rewritten without Reload: the N3 tests above.
- A sublayer changed while the application was closed (N2):
  `USDReadTest.ASublayerRewrittenWhileClosedKeysTheReaderAnew` and
  `USDReadTest.USDReadSublayerChangedWhileClosed`.

## M202 and M71

- M202 gates the image and viewer cache stores. It does not touch `SceneLayerMemo`; M260 fixes N1.
- M71 does not change these caches. `RawDemosaic` keys by `getRenderHash()`, which carries the
  context key.

## Suspected holes

- **N1 Scene layer stored under the key taken before authoring. Fixed in M260.** `produce`
  computes `layerHash` before calling `author`, and authors read knobs live, so an edit landing
  during authoring stored the newer layer under the older hash and undoing the edit served it.
  `produce` now takes a `keyStillValid` check, which `NativeEffectBase::emitSceneLayer` passes:
  it takes the node's key again after authoring. A layer whose key moved is returned to this
  evaluation, under a stack hash of its own, and never memoized, as `CurveTablesCache` does.
- **N2 USDRead sublayers changed while the application was closed. Fixed in M260.**
  `closureDigest` used to be 0 until the first Reload, so sublayers and references were not in
  the hash in a first session, and a persisted DiskCache entry downstream could hit content from
  before the change. The reader now opens its file, walks the files it brings in and stamps them
  (`stampClosure`) when the node is created or loaded (`onEffectCreated`) and when File changes,
  then marks its hash dirty. That is outside any render; evaluations still never re-key the node.
  Reload stamps them again after reloading the layers.
- **N3 Reader caches refreshed while the hash did not. Fixed in M260.** The OIIO header cache and
  the Raw decode caches used to stat the file on each lookup and re-read a changed file, while the
  node hash kept the stamp from first use until Reload, so tiles of two versions mixed under one
  hash. Natron never watches files: a Read pins to the file as last loaded or reloaded. The
  registry records each file's `FileIdentity` (mtime in ns, size, inode) with its stamp, and both
  readers key their caches on it. A reader whose file no longer matches the recorded identity
  fails with "<path> changed on disk since it was loaded, Reload to read it" and never parses or
  decodes it; images already cached under the unchanged hash keep answering. Decodes stat the file
  before opening it and after reading it, so a rewrite during a read fails instead of mixing
  versions. Reload (`NativeRead::purgeCaches`) invalidates the stamp and evicts the header and
  decode, the only way a new version comes in. NativeWrite's read-back of its own output and the
  deep-file sniff in `AppManager` read with `FilePin::eDisk`, unpinned. Note that emptying every
  cache (`AppManager::clearAllCaches`) purges the Reads too, so it reloads their files as Reload
  does. Tests: `FileStamp.*Identity*`, `FileStamp.*MatchesDisk`,
  `FileStamp.OiioReaderFailsOnARewrittenFileUntilReload`,
  `RawDecodeSupportTest.ARewrittenFileFailsWithoutADecodeUntilReload`,
  `CacheStressTest.ReadPinnedUntilReload` and `CacheStressTest.RawReadPinnedUntilReload`.
- **G2** (shader gate ignores the device) is recorded in [gpu.md](gpu.md).
