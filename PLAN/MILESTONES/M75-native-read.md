# M75 - Native Read

Replaces the `Read` container node with a native node on OpenImageIO, for EXR, PNG, JPEG and TIFF, emitting file metadata into the graph. The native node takes the container's own ID, `fr.inria.built-in.Read`, at major 2, the M67 rule (`Engine/Nodes/README.md` "IDs and versions"). The container (`Engine/ReadNode.cpp`, major 1) stays registered under the same ID and is the fallback: node creation picks the major from the file extension in `AppInstance::createNodeInternal`, so video (FFmpeg), RAW and every other OFX-only format keep reading through the container and its embedded openfx-io decoder. A request for an explicit OFX decoder ID (`fr.inria.openfx.ReadOIIO` and so on) still builds the container. M76 - Native Write mirrors this for `fr.inria.built-in.Write`. See `DECISIONS/2026-10-08-native-io-and-metadata-design.md`.

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
  - verify: gtest renders the native Read through `renderRoI` for each format: mipmap 0 bit-exact against OIIO, mipmap 1 equal to `downscaleMipmap` of the exact image. RoD equals the data window on `flat-rgb-offset-window.exr`. An RGB PNG gives 3 components and `flat-alpha-only.exr` gives alpha.
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
  - approach: follow `DESIGN/2026-10-02-project-ocio.md`. The hidden `ocioConfigFile` and `ocioWorkingSpace` strings are set by the existing `Project::pushOCIOConfigToNode` (`:351-366`; `setHostOCIOKnob` works on any node with those knobs); they are part of the hash, so changing the project config or working space re-renders. `ocioInputSpace` is a persistent string, so `reportUnresolvedOCIOColorSpaces` (`:380-423`) checks it unchanged; the UI is a non-persistent `ocioInputSpaceIndex` choice built from `colorSpaceOptions` and rebuilt on `ProjectColorManagement` config change. Defaults apply only on a new node or a user filename edit while `ocioInputSpaceSet` is false: the config's file rules win, otherwise `Project::getFileColorSpace(category)`, with the category taken from the spec per design §4.3 (float/half → float, 16-bit integer → 16-bit, else 8-bit). The conversion is an OCIO CPU processor from input space to working space, cached per (config, input, working), applied to the colour plane's RGB only; equal spaces are a no-op; other planes stay raw.
  - verify: gtest: a float EXR defaults to the working space and stays bit-exact; an 8-bit PNG defaults to the 8-bit space and matches OCIO's own processor on the same pixels; a project working-space change re-renders; a loaded node keeps its saved input space; an unknown space is reported.
  - size: M

- [ ] M75.P1.T7 — Proxy files
  - files: `Engine/Nodes/IO/NativeRead.cpp`, new `Tests/Native/NativeReadProxy_Test.cpp`
  - approach: GenericReader semantics. When the render scale is at or below `proxyThreshold` and `proxy` is set, read the proxy file (same sequence and time rules, through P1.T3's function) instead of downscaling the full file. `originalProxyScale` is computed from the two formats unless `customProxyScale`. RoD and format stay in full-resolution canonical coordinates.
  - verify: gtest with a run-time half-size proxy: mipmap 1 pixels come from the proxy, mipmap 0 from the full file, and the RoD is identical in both.
  - size: M

## Phase 75.2: Metadata, views and integration

- [ ] M75.P2.T1 — Emit file metadata
  - files: `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/OiioReadSupport.cpp`, `Engine/Nodes/Metadata/ImageMetadata.h` (only if a missing helper is needed), new `Tests/Native/NativeReadMetadata_Test.cpp`
  - approach: override the protected `deriveOutputMetadata(time, view)` (`Engine/Nodes/NativeEffectBase.h:349`); `getOutputMetadata` is non-virtual and caches the result. Don't start from `getUpstreamMetadata`: the node has no input. Use the same standard keys and types the container emits for the same file (`Tests/Metadata_Test.cpp:576` pins them): `ofx/filepath` (the resolved frame path), `ofx/frame` (the file frame after P1.T3's mapping), `ofx/framerate`, `ofx/pixelaspect`, `ofx/filesize`, `ofx/mtime`. Add the `ImageSpec` attributes under prefixes: `exr/*`, `exif/*`, timecode, colourspace tags; `oiio:*` is dropped or kept under `oiio/`. Read the attributes from P1.T2's header cache, so the derivation depends only on knobs and the file.
  - verify: gtest: a run-time EXR with custom attributes gives the expected keys and values; JPEG EXIF fields appear under `exif/`; `ofx/frame` follows the time offset; an OFX node downstream sees the same keys through `OfxClipInstance.cpp:1873`; the standard keys equal the container's for the same fixture.
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

- [ ] M75.P2.T2 — Route Read creation by file type
  - files: `Engine/AppInstance.cpp`, `Engine/AppManager.cpp`/`.h`, `Engine/Nodes/IO/NativeRead.cpp`, new `Tests/Native/NativeReadRouting_Test.cpp`
  - approach: **pending the user's per-format decision (see Decisions)**. Replace P1.T1's pin with routing. `AppManager::isNativeReadExtension(ext)` checks one constant set (exr, png, jpg, jpeg, tif, tiff). In `createNodeInternal`, a `PLUGINID_NATRON_READ` request with no serialization and no explicit major takes its filename from the `filename` param default; if there is none and the node would open its dialog (the `Engine/NodeMain.cpp:99` condition), open `openImageFileDialog()` there first and add the result as the default; a cancelled dialog aborts creation, as `ReadNode.cpp:1240` does. A native extension gives major 2, anything else major 1. That routes the Tab menu, Image > Read, `app.createReader`, drag-drop and the command line at one point. Explicit majors and explicit OFX decoder IDs are honoured as they are (the redirect still pins major 1). `getReaderPluginIDForFileType` is unchanged. The native Read's file dialog lists only native extensions; what a filename edited to another extension does is the pending decision.
  - verify: gtest: `.exr`/`.png`/`.jpg`/`.tif` create a `NativeRead`; a `.dpx` creates the container with ReadOIIO; a `.mov` written in the test by the OFX WriteFFmpeg creates the container with ReadFFmpeg and renders; an explicit ReadOIIO request creates the container; each kind keeps its class through save/load; the cross-format filename edit behaves as decided. Full ctest green.
  - size: M

- [ ] M75.P2.T3 — Panel and viewer check under Xvfb
  - files: `Engine/Nodes/IO/NativeRead.cpp` (page layout and labels only), `Tests/gui/m61_uat.py`, `Tests/gui/viewer_error_scrub.py`, new `Tests/gui/m75_read.py`; `Gui/` only if a hook is missing
  - approach: no layer knob on Read (layer-widget design §3). The file's layers reach the viewer's layer menu through the present-layer listing. Lay out the panel as the container does: file, proxy and frame range on Controls, then colourspace. Update the two existing GUI scripts that call `app.createReader`, which now gets the native node for EXR. Run the Xvfb recipe (`build/deeprepro/run-gui.sh`, with `checkForUpdates` pre-seeded off): a Tab-menu Read opens the dialog first; a multilayer EXR lists its layers in the viewer menu, and switching layers changes the image; a `.mov` creates the container.
  - verify: the scripts pass under Xvfb; screenshots of the panel and the layer menu are shared and the user approves them before sign-off.
  - size: M

- [ ] M75.P2.T6 — Document the native Read
  - files: `Engine/Nodes/README.md`
  - approach: add an "I/O nodes" section that summarises the code as built in P1 and P2.T2: Read major 2 vs the container at major 1, the extension set and where routing happens, explicit OFX IDs building the container, knob names shared with GenericReader, and colour through the project OCIO.
  - verify: the section matches the code (`isNativeReadExtension`, `PLUGIN_MAJOR_NATRON_READ`); `lint-ci` green.
  - size: M

**Verification gate:** all four formats decode bit-exactly against OIIO in ctest; file layers, sequences and every time policy, colourspace defaults and conversion, proxy and multi-view are tested; metadata reaches native and OFX nodes, through a Dot as well; routing puts `.exr` on the native Read and `.dpx` and `.mov` on the container, and the `.mov` renders; the Xvfb GUI check with a multilayer EXR passes with screenshots approved by the user; full ctest green; CI (`format`, `lint-ci`, `build-and-test`) green.

## Decisions
- 2026-10-09 — **Briefs refreshed at promotion by a consultant** (the original predated M74's `deriveOutputMetadata` and named stale paths). The native node takes the container's ID `fr.inria.built-in.Read` at major 2; the container stays at major 1 as the fallback, chosen by extension in `AppInstance::createNodeInternal`; explicit OFX decoder IDs still build the container. Rejected: native decoder inside the container under ReadOIIO's ID (keeps the OFX-shaped wrapper and breaks M74's metadata chain), and a hybrid native node embedding an OFX reader (rebuilds the container). P1.T2 split into P1.T2 (decode support) + P1.T4–T7; P2.T4 (Dot), P2.T5 (multi-view), P2.T6 (docs) added. P2.T6 sized M, not S: summarising built code needs judgement.
- 2026-10-09 — **Pending (user):** what to do per currently supported file type, which decides P2.T2's routing set and the cross-format filename-edit behaviour. The user asked for the full list of supported types first.
