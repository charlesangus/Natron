# M75 - Native Read

Replaces the `Read` container node with a native node on OpenImageIO that reads every format OIIO supports, taken from OIIO's runtime `extension_list` minus RAW and a few excluded formats, and emits file metadata into the graph. The native node takes the Read ID `fr.inria.built-in.Read` at major 2, the M67 rule (`Engine/Nodes/README.md` "IDs and versions"), and becomes the only Read: there is no extension fallback. The container (`Engine/ReadNode.cpp`) and every OFX reader are made unreachable and then removed in P2.T14–T18. RAW is dropped, layered documents are deferred to M79 - Layered Document Readers, and there is no video. Write is unchanged until M76 - Native Write. See `DECISIONS/2026-10-08-native-io-and-metadata-design.md` and `DECISIONS/2026-10-09-native-read-format-scope.md`.

## Phase 75.1: Reader core

- [ ] M75.P1.T1 — Node skeleton under the Read ID, with the container pinned for now
  - files: new `Engine/Nodes/IO/NativeRead.h/.cpp`, `Engine/AppManager.cpp` (one `registerBuiltInPlugin<NativeRead>` line beside `ReadNode`'s at `:1663`), `Engine/AppInstance.cpp`, new `Tests/Native/NativeReadSkeleton_Test.cpp`
  - approach: derive from `NativeEffectBase`, modelled on `Engine/Nodes/Deep/DeepRead.cpp`. Give the ID as `PLUGINID_NATRON_READ` (`Engine/EffectInstance.h:102`) and add `PLUGIN_MAJOR_NATRON_READ 2` next to the class. Override: `isReader()`, `isGenerator()` and `isMultiPlanar()` true, `getLayerKnobSpec()` `eNone` (the layer-widget design §3 table gives Read no layer knob), no inputs. Write the knob list in the header comment, using GenericReader's script names so the existing host hooks and the DopeSheet apply unchanged. File and proxy: `filename` (`kOfxImageEffectFileParamName`, a metadata slave), `proxy`, `proxyThreshold`, `originalProxyScale`, `customProxyScale`. Time: `originalFrameRange`, `firstFrame`, `lastFrame`, `before`, `after`, `onMissingFrame`, `frameMode`, `startingTime`, `timeOffset`, `timeDomainUserEdited`, `frameRate`, `customFps`. Colour: `ocioInputSpace`, `ocioInputSpaceIndex`, `ocioInputSpaceSet`, plus hidden `ocioConfigFile` and `ocioWorkingSpace`. Views: hidden `availableViews`. Create `filename` now; later tasks add the rest. `render()` reports a black frame. In `createNodeInternal` (`:1150-1162`), until P2.T2: a `PLUGINID_NATRON_READ` request that doesn't ask for major 2 resolves to major 1. The `isBundledReader` redirect always pins major 1, because it carries the OFX decoder's version and `getPluginBinary` would otherwise resolve that to the native major.
  - verify: gtest: `createNode(Read, 2)` is a `NativeRead`; `createNode(Read)` and `createNode(ReadOIIO)` are `ReadNode` containers; the native node renders black, and a save/load round trip keeps major 2 and the filename. Full ctest is unchanged.
  - size: M

- [ ] M75.P1.T2 — OIIO decode support
  - files: new `Engine/Nodes/IO/OiioReadSupport.h/.cpp`, new `Tests/Native/OiioReadSupport_Test.cpp`
  - approach: no node code. (1) A header cache keyed on (path, mtime, size) holding the `ImageSpec` of every subimage, behind a mutex. (2) `decode(path, subimage, chbegin, chend, RectI window, float* dst, stride)`: reads only the scanlines or tiles that intersect the window, converts to float through OIIO, and flips rows (OIIO is top-down; Natron is bottom-up relative to the data window). No `ImageInput` is shared between threads without its own lock. Every size, stride and offset is `size_t`/`stride_t`; no `int` row arithmetic. No colour conversion here.
  - verify: gtest writes fixtures at run time with `OIIO::ImageOutput` into a temp dir under `build/`: EXR half and float, scanline and tiled, with an offset data window; PNG 8 and 16; JPEG; TIFF 8, 16 and float. It also uses the existing `Tests/fixtures/png-8bit.png`, `png-16bit.png` and `flat-*.exr`. Every pixel of several windows matches OIIO's own `read_image` bit for bit.
  - size: M

- [ ] M75.P1.T4 — Render the colour plane: format, RoD, layout and render scale
  - files: `Engine/Nodes/IO/NativeRead.h/.cpp`, new `Tests/Native/NativeReadDecode_Test.cpp`
  - approach: `getPreferredMetadata`: output format from the display window, kept at (0,0) as `DeepRead.cpp:189` explains; pixel aspect from `PixelAspectRatio`, float depth; colour components follow the file's R/G/B/A (rgba, rgb or alpha per `DESIGN/2026-09-26-rgba-rgb-alpha-layers.md`); a file with none gives a black RGBA colour plane, so there is no implicit shuffle (layer-widget design open question 1). `getRegionOfDefinition` = data window. `render` fills the colour plane through `OiioReadSupport`, split into row bands with `parallelForCancellable` per the README "Threading" rules. Support render scale: at mipmap > 0, decode the level-0 rows behind the window and downscale with `Image::downscaleMipmap` (`Engine/Image.h:713`), so P1.T7 can substitute a proxy. An unreadable file sets a persistent message and returns `eStatusFailed`, and a later good frame clears it (the behaviour `Tests/gui/viewer_error_scrub.py` checks).
  - verify: gtest renders the native Read through `renderRoI` for each format, using fixtures written at run time with `OIIO::ImageOutput` (EXR, PNG, JPEG, TIFF, DPX, TGA, HDR, plus PFM, written by hand if OIIO can't write it): mipmap 0 bit-exact against OIIO, mipmap 1 equal to `downscaleMipmap` of the exact image. RoD equals the data window on `flat-rgb-offset-window.exr`. An RGB PNG gives 3 components and `flat-alpha-only.exr` gives alpha.
  - size: M

- [ ] M75.P1.T5 — File layers as produced planes
  - files: `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/OiioReadSupport.cpp`, new `Tests/Native/NativeReadLayers_Test.cpp`
  - approach: group each subimage's channel names with `LayerRegistry::groupChannelNames` (`Engine/LayerRegistry.cpp:362`), which applies the same rule ReadOIIO and deep reads use. For multi-part EXR, a part name prefixes channels that carry no layer. Report every non-colour layer as produced in `getComponentsNeededAndProduced`, and fill each requested plane in `render`; model the multi-plane output on `Engine/Nodes/Channel/AddLayers` or `Shuffle`. A layer over the 4-channel cap follows the registry's refusal rule (`DESIGN/2026-09-19-layer-registry.md` §1.3). Registration in the project registry comes free from `Node::registerProducedLayers` (`Engine/Node.cpp:6558`); add no reader-specific code.
  - verify: gtest on `flat-three-layers.exr`, `flat-no-color-layers.exr` and a run-time multi-part EXR: every plane matches OIIO; the registry lists the file's layers after creation; a native Grade with a channel-set row on a file layer processes that layer.
  - size: M

- [ ] M75.P1.T3 — Frame sequences, time domain and missing-frame policy
  - files: `Engine/Nodes/IO/NativeRead.cpp`, new `Engine/Nodes/IO/ReadTimeDomain.h/.cpp`, new `Tests/Native/NativeReadTime_Test.cpp`
  - approach: put a pure function in `ReadTimeDomain`: (output time, knob values, frames present) → file frame, black or error. It implements `frameMode` (starting time / time offset), `before`/`after` (hold, loop, bounce, black, error) and `onMissingFrame` (hold previous, hold next, nearest, error, black). Resolve the pattern to a filename with `KnobFile::getFileName` (`Engine/KnobFile.cpp:101`). `originalFrameRange` is filled by the existing `Node::computeFrameRangeForReader` (`Engine/Node.cpp:5706`, called from `Engine/EffectInstance.cpp:5529` for any reader's `filename`). `firstFrame`/`lastFrame` default to it unless `timeDomainUserEdited`. List the frames present with `FileSystemModel::filesListFromPattern` once per pattern change, not per render. `getFrameRange`, frame-varying metadata, and `frameRate` from the file's `FramesPerSecond` with a `customFps` override. The DopeSheet's reader handling works without edits because the ID and knob names match (`Gui/DopeSheet.cpp:245,329`).
  - verify: gtest: a table test of the pure function covering every policy; a node test over a run-time 3-frame sequence with a gap, covering each policy, the time offset and the starting time.
  - size: M

- [ ] M75.P1.T6 — Colourspace through the project OCIO
  - files: `Engine/Nodes/IO/NativeRead.cpp`, new `Engine/Nodes/IO/ReadColorSpace.h/.cpp`, `Engine/Project.cpp`/`.h` (expose the anonymous `colorSpaceOptions`, `:1026`), new `Tests/Native/NativeReadColor_Test.cpp`
  - approach: follow `DESIGN/2026-10-02-project-ocio.md`. The hidden `ocioConfigFile` and `ocioWorkingSpace` strings are set by the existing `Project::pushOCIOConfigToNode` (`:351-366`; `setHostOCIOKnob` works on any node with those knobs); they are part of the hash, so changing the project config or working space re-renders. `ocioInputSpace` is a persistent string, so `reportUnresolvedOCIOColorSpaces` (`:380-423`) checks it unchanged; the UI is a non-persistent `ocioInputSpaceIndex` choice built from `colorSpaceOptions` and rebuilt on `ProjectColorManagement` config change. Defaults apply only on a new node or a user filename edit while `ocioInputSpaceSet` is false: the config's file rules win, then the file's own colourspace tag if it names a space in the config (the user chose to keep ReadOIIO's behaviour), otherwise `Project::getFileColorSpace(category)`, with the category taken from the spec per design §4.3 (float/half → float, 16-bit integer → 16-bit, else 8-bit), for every OIIO format: DPX 10-bit counts as 16-bit integer, HDR and PFM as float. The conversion is an OCIO CPU processor from input space to working space, cached per (config, input, working), applied to the colour plane's RGB only; equal spaces are a no-op; other planes stay raw.
  - verify: gtest: a float EXR defaults to the working space and stays bit-exact; a file with a valid embedded colourspace takes it over the project default; a 10-bit DPX takes the 16-bit default; an 8-bit PNG defaults to the 8-bit space and matches OCIO's own processor on the same pixels; a project working-space change re-renders; a loaded node keeps its saved input space; an unknown space is reported.
  - size: M

- [ ] M75.P1.T7 — Proxy files
  - files: `Engine/Nodes/IO/NativeRead.cpp`, new `Tests/Native/NativeReadProxy_Test.cpp`
  - approach: GenericReader semantics. When the render scale is at or below `proxyThreshold` and `proxy` is set, read the proxy file (same sequence and time rules, through P1.T3's function) instead of downscaling the full file. `originalProxyScale` is computed from the two formats unless `customProxyScale`. RoD and format stay in full-resolution canonical coordinates.
  - verify: gtest with a run-time half-size proxy: mipmap 1 pixels come from the proxy, mipmap 0 from the full file, and the RoD is identical in both.
  - size: M

## Phase 75.2: Metadata, views and integration

- [ ] M75.P2.T1 — Emit file metadata
  - files: `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/OiioReadSupport.cpp`, `Engine/Nodes/Metadata/ImageMetadata.h` (only if a missing helper is needed), new `Tests/Native/NativeReadMetadata_Test.cpp`
  - approach: override the protected `deriveOutputMetadata(time, view)` (`Engine/Nodes/NativeEffectBase.h:349`); `getOutputMetadata` is non-virtual and caches the result. Don't start from `getUpstreamMetadata`: the node has no input. Use the standard keys and types the container emits today (`Tests/Metadata_Test.cpp:576` pins them): `ofx/filepath` (the resolved frame path), `ofx/frame` (the file frame after P1.T3's mapping), `ofx/framerate`, `ofx/pixelaspect`, `ofx/filesize`, `ofx/mtime`. Add the `ImageSpec` attributes under prefixes: `exr/*`, `exif/*`, timecode, colourspace tags; `oiio:*` is dropped or kept under `oiio/`. Read the attributes from P1.T2's header cache, so the derivation depends only on knobs and the file.
  - verify: gtest: a run-time EXR with custom attributes gives the expected keys and values; JPEG EXIF fields appear under `exif/`; `ofx/frame` follows the time offset; an OFX node downstream sees the same keys through `OfxClipInstance.cpp:1873`. The standard keys are asserted as literal expected values copied from `Metadata_Test.cpp:576`'s expectations, not compared against a live container, so the test survives P2.T17.
  - size: M

- [ ] M75.P2.T4 — Metadata through a Dot
  - files: `Engine/Nodes/NativeEffectBase.cpp`, `Engine/OfxClipInstance.cpp`, `Engine/Nodes/README.md`, new `Tests/Native/NativeReadDotMetadata_Test.cpp`
  - approach: closes the Dot half of M74's follow-up; Read → Dot is the common case once Read is native. `getInputEffectMetadata` (`NativeEffectBase.cpp:271`) and the OFX input-clip path (`OfxClipInstance.cpp:1846-1873`) walk through a `NoOpBase` to its input. The fabricated-key fallbacks for unconnected inputs and output clips stay for M77 - Native Metadata Nodes.
  - verify: gtest: native Read → Dot → native Grade, and native Read → Dot → OFX node, both carry the Read's keys; the existing metadata tests are unchanged.
  - size: M

- [ ] M75.P2.T5 — Multi-view EXR
  - files: `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/OiioReadSupport.cpp`, new `Tests/Native/NativeReadViews_Test.cpp`
  - approach: fill the hidden `availableViews` string from the file (the `multiView` attribute, or each part's `view` attribute), so the existing `Node::refreshCreatedViews` (`Engine/Node.cpp:5866`, triggered at `:6100` for readers) creates the project views. `isViewAware()` is true; decode the matching part or channel prefix per view. A single-view file is view-invariant.
  - verify: gtest on a run-time two-view EXR: the project gets both views, the views render different pixels, and a single-view file renders the same for every view.
  - size: M

- [ ] ~~M75.P2.T2 — Route Read creation by file type~~ — cancelled 2026-10-09: the user's format decision removes the container, so nothing is routed by extension. Replaced by P2.T7 (extension set, dialog), P2.T8 (entry points) and P2.T14–T18 (removal).

- [ ] M75.P2.T7 — Native extension set from OIIO, and the create-time file dialog
  - files: `Engine/Nodes/IO/OiioReadSupport.h/.cpp`, `Engine/Nodes/IO/NativeRead.h/.cpp`, new `Tests/Native/NativeReadFormats_Test.cpp`
  - approach: add `OiioReadSupport::readableExtensions()`, computed once (thread-safe static) from `OIIO::get_string_attribute("extension_list")` (format `format:ext,ext;format:…`). Drop whole formats by name with one constant `kExcludedOiioFormats` = {`raw` (RAW cameras, user decision), `null`, `term` (pseudo-formats), `ffmpeg` (no video), `psd` (layered documents move to M79)}. Never hand-list extensions. Lower-case and match case-insensitively; an extension claimed by both an excluded and a kept format stays. Also `formatNameForExtension(ext)` for diagnostics. In `NativeRead`, override `onEffectCreated(mayCreateFileDialog, args)`: when allowed and no filename was given, call `getApp()->openImageFileDialog()`, set the result, and throw on cancel as `ReadNode.cpp:1240-1250` does so creation aborts. A filename outside the set, or one OIIO can't open, sets a persistent "unsupported format" message and fails the render. No routing changes yet.
  - verify: gtest prints the computed set and asserts it contains exr, png, jpg, tif, dpx, cin, tga, hdr, pfm, bmp, sgi, webp; every OIIO `raw` extension is absent unless a kept format claims it; null/term/psd are absent; every member maps to a non-excluded format. A node test: a `.cr2` filename sets the persistent message and doesn't crash. Full ctest unchanged.
  - size: M

- [ ] M75.P2.T8 — Switch every Read entry point to the native node
  - files: `Engine/AppInstance.cpp`, `Engine/AppManager.cpp`/`.h`, `Engine/WriteNode.cpp`, `Tests/Native/NativeReadSkeleton_Test.cpp`, new `Tests/Native/NativeReadEntryPoints_Test.cpp`
  - approach: delete P1.T1's pin block (`AppInstance.cpp:1172-1176`) so unversioned Read requests take the native major. The `isBundledReader` redirect and `kReadContainerMajor` stay until P2.T14, so explicit OFX reader IDs, which tests not yet migrated use, still build the container. Reimplement `getSupportedReaderFileFormats` (`AppManager.cpp:3454`) and `getReaderPluginIDForFileType` (`:3504`) on `OiioReadSupport::readableExtensions()`; the latter returns `PLUGINID_NATRON_READ` or "". That moves the Tab menu, Image > Read (`Gui20.cpp:1276`), `app.createReader` (`AppInstance.cpp:980`), drag-drop (`Gui50.cpp:1254`), file open and command line (`GuiApplicationManager10.cpp:383`, `CLArgs.cpp:524`), PrecompNode (`:561`) and the file-dialog filters with no edits at those sites. Write's read-back (`WriteNode.cpp:762`) now builds a native Read; check the `ocioInputSpace` slave at `:810` still binds (the native knob is a string; if the types don't match, copy the value on change instead). Update the skeleton test's container assertions.
  - verify: gtest: `createNode(Read)` with no major, and `app.createReader` on `.exr`, `.png`, `.dpx`, `.tga`, build a `NativeRead`; `getReaderPluginIDForFileType` returns the Read ID for every member of the set and "" for `cr2` and `mov`; supported formats equal the set; a Write with read-back renders through a native Read; save/load keeps the native class. Full ctest green; `tools/ci/local/test.sh smoke debug` passes.
  - size: M

- [ ] M75.P2.T9 — Migrate tests to the native Read: shared base and layer/shuffle batch
  - files: `Tests/BaseTest.h/.cpp`, `Tests/AddLayers_Test.cpp`, `Tests/AddLayersRender_Test.cpp`, `Tests/RemoveLayers_Test.cpp`, `Tests/RemoveLayersRender_Test.cpp`, `Tests/Shuffle_Test.cpp`, `Tests/ShuffleRender_Test.cpp`, `Tests/ShuffleMatrix_Test.cpp`
  - approach: add `_readPluginID = PLUGINID_NATRON_READ` to BaseTest beside `_readOIIOPluginID` (removed in P2.T14). Swap the reader ID in each file (`ShuffleMatrix_Test.cpp:189` uses `PLUGINID_OFX_READOIIO` directly); same `filename` knob. Each failing assertion is either a native bug, fixed in `NativeRead`, or a container-specific expectation, rewritten and justified in the commit message. Runs after P1 and P2.T1/T4/T5/T8.
  - verify: full ctest green; no `_readOIIOPluginID` left in the batch's test files.
  - size: M

- [ ] M75.P2.T10 — Migrate tests: channel and tool-layer batch
  - files: `Tests/ChannelSetRender_Test.cpp`, `Tests/LayerKnobs_Test.cpp`, `Tests/LayerKnobsRender_Test.cpp`, `Tests/GeneratorLayer_Test.cpp`, `Tests/RotoLayer_Test.cpp`, `Tests/TrackerLayer_Test.cpp`
  - approach: as P2.T9, using `_readPluginID`. Independent of P2.T11–T13.
  - verify: full ctest green; no `_readOIIOPluginID` left in these files.
  - size: M

- [ ] M75.P2.T11 — Migrate tests: registry, colour-view, deep and time batch
  - files: `Tests/LayerRegistry_Test.cpp`, `Tests/ColorViewsRender_Test.cpp`, `Tests/DeepLayers_Test.cpp`, `Tests/TimeVaryingLayers_Test.cpp`
  - approach: as P2.T9. Delete `ReadEmbeddedDecoderPlaneKnobsHiddenAndPinnedToColor` (`LayerRegistry_Test.cpp:598-625`): the native Read has no decoder and no layer knob, which P1.T1's test covers. In `ColorViewsRender_Test`, drop `kReadPNGPluginID`/`kReadEXRPluginID` (`:90-91`) and point the alpha-only PNG/EXR round trips (`:989`, `:1052`) at the native Read.
  - verify: full ctest green; no `_readOIIOPluginID`, `ReadNode` or `fr.inria.openfx.Read` left in these files.
  - size: M

- [ ] M75.P2.T12 — Migrate tests: I/O, project and engine batch
  - files: `Tests/WriteAllLayers_Test.cpp`, `Tests/SchedulerWriters_Test.cpp`, `Tests/ProjectSerialization_Test.cpp`, `Tests/PersistentMessage_Test.cpp`, `Tests/ReadFormat_Test.cpp`, `Tests/Native/EngineHooks_Test.cpp`, `Tests/Native/NativeImageEffect_Test.cpp`
  - approach: as P2.T9. `ReadFormat_Test.cpp:99`'s "backed by a Read container" check becomes a `NativeRead` check. WriteOIIO stays everywhere (Write is M76's job).
  - verify: full ctest green; no `_readOIIOPluginID` or `ReadNode` left in these files.
  - size: M

- [ ] M75.P2.T13 — Migrate tests: metadata and OCIO batch
  - files: `Tests/Metadata_Test.cpp`, `Tests/OfxMetadataBridge_Test.cpp`, `Tests/ProjectOCIO_Test.cpp`, `Tests/ProjectOCIODefaults_Test.cpp`, `Tests/ProjectOCIOPlugins_Test.cpp`
  - approach: Metadata_Test: delete `ReaderOutputClipCarriesPerFrameFileMetadata` (`:576-657`, decoder output clip; P2.T1 covers it) and point `ReaderFileMetadataReachesADownstreamInputClip` (`:664`) at the native Read. OfxMetadataBridge_Test: in `ReadKeysReachTheWriteEncoderThroughNativeNodes` (`:365`) replace the `dynamic_cast<ReadNode*>` (`:404`) with a `NativeRead` check. ProjectOCIO_Test: the reader helper (`:92-105`) builds the native Read and reads `ocioConfigFile`/`ocioWorkingSpace` off it; delete `ANewDecoderAfterAFormatChangeCarriesTheProjectConfig` (`:355`). ProjectOCIODefaults_Test: port the Read cases (`:356-537`) to the native Read with the same knob names and `ocioInputSpaceSet` semantics, **including** `ReadOIIOKeepsTheFilesOwnValidColorspaceOverTheProjectDefault` (`:393`, renamed for the native Read; the user chose to honour the file's tag). ProjectOCIOPlugins_Test: take the OFX instance-property checks from WriteOIIO's embedded encoder instead of ReadOIIO's decoder. Write cases untouched.
  - verify: full ctest green; `grep -rn '_readOIIOPluginID\|PLUGINID_OFX_READ\|ReadNode\b' Tests` finds only `BaseTest`.
  - size: M

- [ ] M75.P2.T14 — Make the container and the OFX readers unreachable
  - files: `Engine/AppInstance.cpp`, `Engine/AppManager.cpp`, `Engine/OfxHost.cpp`, `Tests/BaseTest.h/.cpp`, `Tests/Native/NativePluginList_Test.cpp`, `tools/ci/smoke_test.py`, `Tests/fixtures/read-time-offset.ntp`
  - approach: delete the `isBundledReader` redirect branch (`AppInstance.cpp:1160-1164`) and `kReadContainerMajor` (`:1128-1130`); remove `registerBuiltInPlugin<ReadNode>` (`AppManager.cpp:1664`) — `ReadNode` stays compiled but dead until P2.T17. In `OfxHost::loadOFXPlugins`, skip any plugin whose contexts include `kOfxImageEffectContextReader` (before `:1070`). Drop `_readOIIOPluginID` from BaseTest and `_allTestPluginIDs` (`:81-82`). Append Read to `NativePluginList_Test` (one version, the native major); assert no registered plugin is a reader except `PLUGINID_NATRON_READ`, and `createNode("fr.inria.openfx.ReadOIIO")` builds nothing. `smoke_test.py`'s IO representative (`:302`) becomes WriteOIIO only. Regenerate `read-time-offset.ntp` with a native Read with `timeOffset` 50 (no backward compatibility; don't hand-convert).
  - verify: full ctest green; `tools/ci/local/test.sh smoke debug` passes, including the `NatronRenderer -i Read1` check.
  - size: M

- [ ] M75.P2.T15 — Strip container branches from Engine code
  - files: `Engine/Node.cpp`, `Engine/NodeMain.cpp`, `Engine/AppInstance.cpp`, `Engine/Project.cpp`, `Engine/Nodes/Metadata/OfxMetadataBridge.cpp`, `Engine/OfxParamInstance.cpp`, `Engine/OfxEffectInstance.cpp`
  - approach: deletion only. Drop the Read halves at `Node.cpp:899,1740,3427,3757,3874,4417`, `NodeMain.cpp:115`, `AppInstance.cpp:1487` and the dead `#ifndef` block at `:984`, `Project.cpp:355`, `OfxMetadataBridge.cpp:190` (keep the WriteNode embedded-encoder lookup), `OfxParamInstance.cpp:323` (keep `isBundledWriter`). `OfxEffectInstance.cpp:1440` `isVideoReader` returns false, or goes if nothing else overrides it. Write branches untouched. Parallel with P2.T16.
  - verify: build; full ctest green; the M74 metadata tests and Write tests are unchanged.
  - size: M

- [ ] M75.P2.T16 — Strip container branches from Gui code
  - files: `Gui/DockablePanel.cpp`, `Gui/DocumentationManager.cpp`, `Gui/DopeSheet.cpp`, `Gui/DopeSheetEditorUndoRedo.cpp`, `Gui/Gui20.cpp`, `Gui/SequenceFileDialog.cpp`, `Gui/Gui.cpp`
  - approach: deletion only. Remove the `ReadNode` includes and dynamic-cast branches (`DockablePanel.cpp:141`, `DocumentationManager.cpp:281`) and the reader half of `DocumentationManager.cpp:186`. Delete the never-compiled `#ifndef NATRON_ENABLE_IO_META_NODES` branches at `DopeSheet.cpp:242,326`, `DopeSheetEditorUndoRedo.cpp:112`, `Gui20.cpp:1298`, `SequenceFileDialog.cpp:2695`, and the Windows-only reader-format loop at `Gui.cpp:92`. Parallel with P2.T15.
  - verify: build; full ctest green; `grep -rn ReadNode Gui` is empty.
  - size: M

- [ ] M75.P2.T17 — Delete ReadNode and the reader-plugin map
  - files: delete `Engine/ReadNode.h/.cpp`; `Engine/EffectInstance.h/.cpp`, `Engine/PyNodeGroup.cpp`, `Engine/WriteNode.cpp`, `Engine/AppManager.cpp/.h`, `Engine/AppManagerPrivate.h`, `Engine/OfxHost.cpp/.h`, `Gui/GuiApplicationManager.h`, `Gui/GuiApplicationManager10.cpp`, `Engine/Nodes/IO/NativeRead.h`
  - approach: compile-driven deletion after P2.T15/T16. Remove the files (Engine globs sources; reconfigure), `friend class ReadNode` (`EffectInstance.h:2627`), the includes, and the now-unused `PLUGINID_OFX_READ*` defines (`EffectInstance.h:55-85`). Move `kNatronReadNodeOCIOParamInputSpace` into `NativeRead.h` for `WriteNode.cpp:810`. Delete `readerPlugins`, `getFileFormatsForReadingAndReader`, `getReadersForFormat` and the `readersMap` parameter through `loadBuiltinNodePlugins` and `loadOFXPlugins`. Fix the `NativeRead.h:42-43` comment: Read is the only plugin under its ID.
  - verify: clean reconfigure and build; full ctest green; `grep -rn 'ReadNode\b\|readerPlugins\|isBundledReader' Engine Gui Tests` is empty.
  - size: M

- [ ] M75.P2.T18 — Retire the OFX readers from the plugin bundle
  - files: `tools/ci/local/fetch-assets.sh`, `tools/ci/smoke_test.py`, and `CMakeLists.txt` in the forks `charlesangus/openfx-io` and `charlesangus/openfx-arena`
  - approach: follow M67.P2.T10. Fork branch `m75/retire-readers` in openfx-io drops ReadOIIO (keeping WriteOIIO and OIIOText), ReadEXR, ReadPNG and ReadPFM; WritePNG/WritePFM stay for M76. In openfx-arena drop ReadPSD, ReadMisc, ReadKrita and OpenRaster (`CMakeLists.txt:159-179`), keep the ImageMagick effects, and point arena's OpenFX-IO submodule at the new openfx-io head. Re-pin `OPENFX_IO_REF`/`OPENFX_ARENA_REF` to the branch heads, then to the merge commits (merge-committed, not squashed) when the fork PRs merge with M75. Add a comment line in the style of the M67 lines saying the native Read replaces them. `smoke_test.py`'s Arena representatives must not be readers.
  - verify: after the PM reruns `fetch-assets.sh` in the container, `verify_plugin_loads` passes for IO and Arena; full ctest green; smoke passes.
  - size: M

- [ ] M75.P2.T3 — Panel and viewer check under Xvfb
  - files: `Engine/Nodes/IO/NativeRead.cpp` (page layout and labels only), `Tests/gui/m61_uat.py`, `Tests/gui/viewer_error_scrub.py`, new `Tests/gui/m75_read.py`; `Gui/` only if a hook is missing
  - approach: no layer knob on Read (layer-widget design §3); the file's layers reach the viewer's layer menu through the present-layer listing. Panel layout as the container had it: file, proxy and frame range on Controls, then colourspace. Check the two existing GUI scripts that call `app.createReader`. Run the Xvfb recipe (`build/deeprepro/run-gui.sh`, `checkForUpdates` pre-seeded off): a Tab-menu Read opens the dialog first, and its filter lists DPX and TGA but not CR2; a multilayer EXR lists its layers in the viewer menu and switching changes the image; a run-time DPX and TGA each display through a native Read; searching "ReadOIIO" or "ReadPNG" in the Tab menu finds nothing.
  - verify: the scripts pass under Xvfb; screenshots of the panel, the layer menu and the DPX in the viewer are shared and the user approves them before sign-off.
  - size: M

- [ ] M75.P2.T6 — Document the native Read
  - files: `Engine/Nodes/README.md`
  - approach: an "I/O nodes" section summarising the code as built: Read is native only, at `PLUGIN_MAJOR_NATRON_READ` with a single version; the extension set comes from OIIO's `extension_list` minus `kExcludedOiioFormats`, with no hand list; every entry point goes through `getReaderPluginIDForFileType`/`getSupportedReaderFileFormats`; `OfxHost` refuses reader-context OFX plugins; knob names are shared with GenericReader; colour goes through the project OCIO and honours a valid embedded colourspace; RAW is dropped and layered documents are deferred to M79. Fix the stale "Read/Write container" wording at `:120` to Write only.
  - verify: the section matches the code (`readableExtensions`, `kExcludedOiioFormats`, `PLUGIN_MAJOR_NATRON_READ`, the OfxHost skip); `lint-ci` green.
  - size: M

**Verification gate:** EXR, PNG, JPEG, TIFF, DPX, TGA, HDR and PFM decode bit-exactly against OIIO in ctest; file layers, sequences and every time policy, colourspace defaults (including a file's own valid tag) and conversion, proxy and multi-view are tested; metadata reaches native and OFX nodes, through a Dot as well; every extension in OIIO's runtime `extension_list` outside the excluded formats (raw, null, term, ffmpeg, psd) creates a native Read through every entry point, and RAW-only extensions are refused; no OFX reader is registered or reachable, `ReadNode` is gone from the tree, and the fork bundles no longer build OFX readers while WriteOIIO still works; the Xvfb GUI check with a multilayer EXR and a DPX passes with screenshots approved by the user; full ctest green; smoke green; CI (`format`, `lint-ci`, `build-and-test`) green.

## Decisions
- 2026-10-09 — **Briefs refreshed at promotion by a consultant** (the original predated M74's `deriveOutputMetadata` and named stale paths). The native node takes the container's ID `fr.inria.built-in.Read` at major 2; the container stays at major 1 as the fallback, chosen by extension in `AppInstance::createNodeInternal`; explicit OFX decoder IDs still build the container. Rejected: native decoder inside the container under ReadOIIO's ID (keeps the OFX-shaped wrapper and breaks M74's metadata chain), and a hybrid native node embedding an OFX reader (rebuilds the container). P1.T2 split into P1.T2 (decode support) + P1.T4–T7; P2.T4 (Dot), P2.T5 (multi-view), P2.T6 (docs) added. P2.T6 sized M, not S: summarising built code needs judgement.
- 2026-10-09 — **Format scope (user):** every OIIO format goes native, from OIIO's `extension_list` minus RAW; RAW is dropped; the container path is removed, with no fallback, no xpm/miff and no video. P2.T2 and the gate are being re-planned. See `DECISIONS/2026-10-09-native-read-format-scope.md`.
- 2026-10-09 — **Re-planned for the container's removal** (consultant). Read keeps major 2: M67's convention of a single native version at OFX major + 1, and `getPluginBinary` resolves stale major-1 requests upward. P2.T2 is cancelled. Order: native features, then P2.T7 (OIIO extension set and dialog), P2.T8 (entry points; pin removed), P2.T9–T13 (tests migrated in batches while the redirect still serves unmigrated ones), P2.T14 (unreachable; `OfxHost` skips reader-context plugins), P2.T15–T17 (delete), and P2.T18 (forks drop the readers). `psd` is excluded from the OIIO set, following the user's move of Photoshop to M79. P2.T15/T16 were sized M, not the consultant's S: compile-driven deletions across seven files.
- 2026-10-09 — **File colourspace tag honoured (user):** order is config file rules → the file's valid colourspace tag → project default for the bit depth. ReadOIIO's test is ported in P2.T13 rather than deleted.
