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
| `SceneLayerMemo` (authored USD layer) | Nodes/Scene/SceneLayerMemo.h:40-110, `produce` Nodes/Scene/SceneOps.cpp:357-440 | `layerHash`: `nodeDigest`, upstream-read mode, the upstream stack hash or `factsDigest`, frame range if read, fps (:365-385) | yes, during authoring (N1) | no; facts mode is as right as each node's `factsDigest` | via the stamp model (USDRead N2) |
| `CurveTablesCache` | Nodes/Image/CurveSnapshot.h:384-424; users ColorCorrect.h:161, HueCorrect.h:167, ColorLookup.h:157 | `knob.getContentHash()`, stored only when the hash is unchanged after the build | only through E1 | n/a | n/a |
| `StageGpuShaderGate` | Nodes/Image/StagePlan.h:415-427, member Nodes/Image/StagedImageEffect.h:543 | entry + source (Nodes/Image/StagePlan.cpp:1639-1644) | no | no | n/a; ignores the device (G2) |
| AddLayers registry signature | Nodes/Channel/AddLayers.h:218, Nodes/Deep/DeepAddLayers.h:156 | layer registry signature, folded into `appendNodeContentHash` (AddLayers.cpp:360-379) with `markHashDirty` on change | no | no | n/a |
| NativeRead frame listings | `_listing`, `_proxyListing` Nodes/IO/NativeRead.h:260-262, built NativeRead.cpp:528-591 | canonical pattern, with an invalidation-counter guard | no (cleared on filename/proxy edits :829, :987) | n/a | until Reload (`purgeCaches` :609-650), same as the stamp |
| USDRead file layer and closure | `Implementation` Nodes/Scene/USDRead.cpp:81-100 | `closureDigest` (stamps of every file brought in) in `appendNodeContentHash` (:273-277) | no | n/a | yes in a first session (N2) |
| NativeWrite read-back files | `_readBackFiles` Nodes/IO/NativeWrite.h:347-349, NativeWrite.cpp:1425-1470, purge :1564-1590 | output path, validated against the stored output hash (:1430) | no | no | until purge |
| USDTransform matches memo | Nodes/Scene/USDTransform.cpp:142-157 | (matched stack hash, pattern, op name) | no | no | n/a |
| RawDemosaic capture and green memos | Nodes/Raw/RawDemosaic.h:265, RawDemosaic.cpp:980-1004, :1072-1080 | `RawNodeSupport::frameKey(getRenderHash(), time, view)`, stored only if not cancelled | no | no | as the decode cache (N3) |
| IErodeDilate half widths | `static thread_local` Nodes/Filter/IErodeDilate.cpp:585, class :85-110 | (rx, ry); memo of a pure function | no | no | n/a |
| OIIO header cache | Nodes/IO/OiioReadSupport.cpp:63-80, :145-221 | (path, mtime, size) with an eviction counter; `evictHeaders` :213 | no | n/a | refreshes itself while the hash does not (N3) |
| Raw decode caches | Nodes/IO/RawDecodeSupport.cpp:78-131, :730-760 | `FileKey` (path, mtime, size) :80-88; `evict` :906 | no | n/a | as OIIO (N3) |
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

- A file changed on disk without Reload, by design (the stamp model). N3 is where a node-private
  cache does not follow that design.
- A device switch, for `StageGpuShaderGate` (G2).

## Covered by tests

- Scene layer memo: `Tests/Scene/SceneLayerMemo_Test.cpp:93-171` (reuse, changed key, changed
  upstream, failed authoring, invalidate) and the chain tests at :357-577 (re-authoring only the
  edited layer, facts-reading ops). No test edits a knob while a layer is being authored.
- Reload: `Tests/Native/NativeReadReload_Test.cpp`.
- No test changes a sublayer on disk before the first Reload, or rewrites a file read by OIIO or
  Raw without Reload.

## M202 and M71

- M202 gates the image and viewer cache stores. It does not touch `SceneLayerMemo`, so N1 stays.
  N3's images are stored under an unchanged hash, which M202's snapshot does not catch.
- M71 does not change these caches. `RawDemosaic` keys by `getRenderHash()`, which carries the
  context key.

## Suspected holes

- **N1 Scene layer stored under the key taken before authoring.** `produce` computes `layerHash`
  before calling `author` (SceneOps.cpp:365-385), and authors read knobs live (USDPrim.cpp:156-158).
  Nothing rechecks the key after authoring, unlike `CurveTablesCache`. An edit landing during
  authoring stores the newer layer under the older hash; undoing the edit then serves it.
  **Not fixed by M202.**
- **N2 USDRead sublayers changed while the application was closed.** `closureDigest` is 0 until the
  first `purgeCaches` (USDRead.cpp:95-99, :243-271). The main file is stamped by its File knob, but
  sublayers and references are not in the hash in a first session. A persisted DiskCache entry
  downstream can then hit content from before the change. **Not fixed by M202.**
- **N3 Reader caches refresh while the hash does not.** The OIIO header cache and the Raw decode
  caches stat the file on each lookup (OiioReadSupport.cpp:146-147, RawDecodeSupport.cpp:153-155)
  and re-read a changed file. The node hash keeps the stamp from first use until Reload
  (FileStampRegistry.h:40-47). New tiles of a changed file are stored beside old ones under one
  hash and mix until Reload. Their keys also omit the inode, which the stamp includes.
  **Not fixed by M202. Deferred from M242:** keying the reader caches on the stamp alone would leave
  an old header beside pixels read live from the changed file, which is worse. A fix needs a
  decision between the reader pinning the file to its stamp (refusing or re-stamping on a change)
  and the node noticing the change and moving its hash. On the follow-up list.
- **G2** (shader gate ignores the device) is recorded in [gpu.md](gpu.md).
