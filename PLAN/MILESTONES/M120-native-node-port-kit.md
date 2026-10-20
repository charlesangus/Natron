# M120 - Native Node Port Kit

Make the next round of native ports runnable side by side. Those ports are M113 - Native Warp Nodes and the port milestones created alongside this one on 2026-10-13. Every port today edits the same shared manifests:
- `AppManager::loadBuiltinNodePlugins()`
- the Slang kernel list
- `Engine/GpuCostSeed.inc`
- `GuiResources.qrc`
- `NativePluginList_Test.cpp`'s table
- `Engine/Nodes/README.md`
- the openfx-misc fork pin used to retire plug-ins

Because of these, PLAN-FORMAT §8a's file-overlap test would serialise every port. This milestone splits each manifest into **per-node fragment files** that CMake globs, and makes OFX retirement one marker file per retired OFX ID. A port then touches only files named after its own nodes and the IDs it retires. Per-family files would not be enough: several families have two or three sibling milestones that are meant to run in parallel lanes — Filter (M123 - Native Sharpen And Glow, M130 - Native Edge-Preserving And Directional Filters, M131 - Native Defocus Family), Generator (M128 - Native Shape Generators, M129 - Clean-Sheet Noise Node), Time (M124 - Native Time Remaps, M125 - Native Multi-Frame Nodes) and Transform (M113 - Native Warp Nodes, M132 - Native Lens Distortion) — and a shared family file would make them overlap. It also builds an in-repo, test-only OFX bundle of fixture plug-ins and moves the tests that use soon-to-be-retired OFX plug-ins as fixtures onto it, so no future port breaks them.

Deps: M85 - Native GPU Kernels (its `slang_add_kernel` lines and cost seeds are migrated here).

Scouted facts (scouted 2026-10-13 on main `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion):
- **Registration:** one `registerBuiltInPlugin<T>(icon, deprecated, internal)` line per node in `Engine/AppManager.cpp:1625-1674`. The template is just above. Icons are `NATRON_IMAGES_PATH "NativeNodes/<ofx-id>.png"`.
- **Node directories:** `Engine/Nodes/{Channel,Color,Deep,Filter,Generator,IO,Image,Keyer,Merge,Metadata,Transform}/`. `Engine/CMakeLists.txt:43-46` GLOB_RECURSEs `Engine/Nodes/**`.
- **GPU kernels:** M85 - Native GPU Kernels's `Engine/Gpu/CMakeLists.txt:18-34` has one `slang_add_kernel(<id> Kernels/<file>.slang ENTRY <entry> REGISTRY NatronGpu)` line per kernel. The registry generates `gpu::GpuKernelId`.
- **Cost seeds:** `Engine/GpuCostSeed.inc` is included by `Engine/GpuCostModel.cpp:64`. It is emitted by `tools/bench/render-profile-report.py --emit-cost-table` (header comment at `:13`).
- **Icons:** hand-listed in `Gui/GuiResources.qrc:383-` (`Resources/Images/NativeNodes/*.png`). The same list is at `GuiResources.qrc:409-415` for the filters.
- **Icon names:** the 33 icons in `Gui/Resources/Images/NativeNodes/` are named after the OFX ID they replace (`net.sf.openfx.GradePlugin.png`). They keep those names here; M111 - Native Node Identity Cleanup renames them.
- **OFX Support library:** `libs/OpenFX/Support/include/ofxsImageEffect.h` and the rest are in-repo, so the test bundle can use the C++ Support layer.
- **Plug-in list test:** `Tests/Native/NativePluginList_Test.cpp:80` hand `nativeTable()`, iterated at `:134, :144`. `Tests/CMakeLists.txt:120` already GLOBs `Native/*_Test.cpp`, so new native tests need no CMake edit.
- **OFX loading:** `OfxHost::loadOFXPlugins()` (`Engine/OfxHost.cpp:845-1055`) iterates `getPluginsByIDMajor()` and already skips reader and writer contexts (`:946-953`). This is the natural place for a retired-ID skip. Bundles hold many plug-ins per `.so` (`Misc.ofx`, `CImg.ofx`, `IO.ofx`), so single plug-ins cannot be dropped from a binary without a fork commit (`tools/ci/local/fetch-assets.sh:139-165`, `OPENFX_MISC_REF`).
- **Tests that use to-be-retired OFX IDs as fixtures** (scout file:line; see the table below):
  - `Tests/DefaultChannelSet_Test.cpp:59-143,271`
  - `LayerKnobs_Test.cpp:97,282-289,579`
  - `LayerKnobsRender_Test.cpp:472-922`
  - `ColorViewsRender_Test.cpp:90-93,849,892`
  - `ProjectSerialization_Test.cpp:553-555`
  - `ChannelSetRender_Test.cpp:74,1286-1369`
  - `Metadata_Test.cpp:89-91`
  - `NodeCategory_Test.cpp:168,189-192`
  - `Native/EngineHooks_Test.cpp:309-351`
  - `GeneratorLayer_Test.cpp:67,316`
  - `SchedulerEquivalence_Test.cpp:82,411,773,860`
  - `TimeVaryingLayers_Test.cpp:219-297,474-482`
  - `ShuffleRender_Test.cpp:253,1315`
  - `RemoveLayersRender_Test.cpp:164,815`
  - `PersistentMessage_Test.cpp:137`
  - `FrameGraphBuild_Test.cpp:74,514-548`
  - `GpuResidency_Test.cpp:828-836`
  - `ProjectOCIO_Test.cpp:65-69,288-`, `ProjectOCIOPlugins_Test.cpp:46,72`
  - `GLScheduler_Test.cpp:69,296`
  - `KnobLayerSelect_Test.cpp:695`
  - `PluginIdentity_Test.cpp:51`
  - `OfxMetadataBridge_Test.cpp:76-77` (ColorMatrix; found 2026-10-19)
  - `tools/ci/smoke_test.py:303-305,688-690`

**OFX IDs this round retires.** Each port milestone adds one `Engine/OfxRetired/<ofx-id>` marker per ID it retires:

| Milestone | IDs |
|---|---|
| M121 - Native Routing And Mix | `net.sf.openfx.switchPlugin`, `net.sf.openfx.KeyMix`, `net.sf.openfx.DifferencePlugin`, `net.sf.openfx.AdjustRoDPlugin` |
| M122 - Native Colour Utilities | `net.sf.openfx.Premult`, `.Unpremult`, `.HSVToolPlugin`, `.HueCorrect`, `.HueKeyer`, `.ColorMatrixPlugin`, `.ColorSuppress`, `.Despill`, `.Threshold`, `.Quantize`, `.Log2Lin`, `.PLogLin`, and every `ColorTransform` converter (`RGBToHSV` … `XYZToRGB709`, HSL/HSI/YCbCr/YPbPr/YUV/Lab/xyY) |
| M123 - Native Sharpen And Glow | `net.sf.cimg.CImgSharpen`, `CImgSoften`, `CImgLaplacian`, `CImgBloom`, `eu.cimg.EdgeExtend` |
| M130 - Native Edge-Preserving And Directional Filters | `net.sf.cimg.CImgMedian`, `CImgBilateral`, `CImgBilateralGuided`, `CImgGuided`, `CImgRollingGuidance`, `net.sf.openfx.DirBlur`, `net.sf.openfx.GodRays` |
| M124 - Native Time Remaps | `net.sf.openfx.timeOffset`, `.FrameHold`, `.FrameRange`, `.Retime` |
| M125 - Native Multi-Frame Nodes | `net.sf.openfx.FrameBlend`, `.TimeBlur`, `.NoTimeBlurPlugin`, `.TimeDissolvePlugin`, `.AppendClip`, `.SlitScan`, `.TimeBufferRead`, `.TimeBufferWrite` |
| M113 - Native Warp Nodes | `net.sf.openfx.IDistort`, `.STMap`, `.CornerPinPlugin`, `.CornerPinMaskedPlugin`, `.Mirror` |
| M132 - Native Lens Distortion | `net.sf.openfx.LensDistortion` |
| M127 - Native OCIO Nodes | `fr.inria.openfx.OCIOColorSpace`, `OCIODisplay`, `OCIOCDLTransform`, `OCIOFileTransform`, `OCIOLogConvert`, `OCIOLookTransform` |
| M128 - Native Shape Generators | `net.sf.openfx.Ramp`, `.Radial`, `.Rectangle`, `.ColorWheel` |
| M129 - Clean-Sheet Noise Node | `net.sf.openfx.Noise`, `net.sf.cimg.CImgNoise`, `CImgPlasma` |
| M140 - Native Metadata Utilities | `org.openfx.examples.metadataCompare`, `metadataContribute`, `metadataTimeCode` |

**Staying OFX for now** (no longer used as test fixtures; the smoke test may still name them): `NoOpPlugin`, `CopyRectanglePlugin`, `ColorBars`, `ContactSheetOFX`, `LayerContactSheetOFX`, `Card3D`, `ImageStatistics`, `Deinterlace`, `DenoiseSharpen`, the arena nodes, and the remaining CImg filters (Smooth, Denoise, SharpenShock, SharpenInvDiff, RollingGuidance, Equalize, HistEQ, ChromaBlur, ErodeSmooth, ErodeBlur, Inpaint, Distance, Matrix, Expression).

## Scope

**In:**
- Per-node fragment files for registration, GPU kernels, cost seeds, docs and plug-in-list expectations, plus a globbed icon resource, with every existing native node migrated onto them. The scheme every port milestone uses:
  - registration: `Engine/Nodes/<Family>/<Node>.reg.inc` (one X-macro line naming the class and its header; identity, icon and flags come from the class's `NodeSpec`, from M137 - Native Base Class Flexibility);
  - GPU kernels: `Engine/Gpu/Kernels/<kernel>.cmake` beside each `<kernel>.slang`, holding its `slang_add_kernel` lines;
  - GPU cost seeds: `Engine/Nodes/<Family>/<Node>.costseed.inc`;
  - docs: `Engine/Nodes/<Family>/docs/<Node>.md`, including that node's tolerance line;
  - plug-in identity: each node's own `Tests/Native/Native<Node>_Test.cpp` asserts its ID, major and label; `NativePluginList_Test` checks the registry against the set of `.reg.inc` fragments, so no shared expectation table exists;
  - cost seeds of OFX plug-ins not yet ported: `Engine/GpuCostSeeds/ofx/<plugin-id>.inc`, deleted by the milestone that retires the plug-in;
  - OFX retirement: `Engine/OfxRetired/<ofx-id>`, one marker file per retired ID, holding at most one line, the native ID that replaces it;
  - icons: `Gui/Resources/Images/NativeNodes/<Node>.png`, globbed.
- A host-side retired-ID mechanism, compiled in from the marker files, with its test.
- An in-repo, test-only OFX bundle (`Tests/OfxTestPlugins/`) of fixture plug-ins, built only into the test tree and never shipped. M140 - Native Metadata Utilities adds its metadata plug-ins to it.
- Test fixtures moved off the IDs in the table and onto the test bundle.
- An "Adding a native node" checklist in `Engine/Nodes/README.md`, naming every fragment a port adds.

**Out:**
- Renaming existing native nodes to plain IDs and the README native-node policy rewrite: M111 - Native Node Identity Cleanup.
- Behaviour fixes: M112 - Native Node Behaviour Fixes.
- Deleting retired plug-ins from the openfx-misc and openfx-io forks, and removing the skip mechanism afterwards: M145 - OFX Fork Plugin Purge.
- Retiring any ID: each port milestone adds its own markers.
- The metadata test plug-ins (Contribute, Print, TimeCode): M140 - Native Metadata Utilities.
- Splitting plug-in groups (`Global/Enums.h`, `Macros.h`, `GuiApplicationManager.cpp`), `tools/ci/smoke_test.py`'s ID lists and `fetch-assets.sh`: they stay shared, since new groups are rare and the §8a overlap check serialises the odd case. None.
- A GL-capable test plug-in: none. `GLScheduler_Test`'s OCIO fixture stays, marked for M127 - Native OCIO Nodes.
- Renaming icon files: M111 - Native Node Identity Cleanup.

**Edge cases:**
- A retired ID requested by an old project or script loads the missing-plug-in node, the same as any absent plug-in (clean break).
- A glob that matches nothing (no kernels or seeds yet in a directory) yields an empty aggregate, and the build tolerates that.
- The cost-seed emitter must still regenerate seeds. It writes one `<Node>.costseed.inc` per node, locating the node's family dir from its `.reg.inc`.
- Two milestones adding nodes at once each add new fragment files only; neither edits an aggregate, so their branches merge without conflict.
- A marker for an ID that no bundled plug-in provides (a typo) fails `OfxRetired_Test`.
- Two fragments registering the same native ID fail `NativePluginList_Test`.
- A retired ID that is also a native ID, or a marker naming a replacement that isn't registered, fails `NativePluginList_Test`.
- A fixture test run without `OFX_PLUGIN_PATH` set still finds the test bundle.

**Assumptions:**
- `Tests/wmain.cpp` can add the test bundle's directory to `OFX_PLUGIN_PATH` before `AppManager::load()`, because the plug-in registry is filled at load (scouted by M140 - Native Metadata Utilities's planner; unverified).

## Phase 120.1: Per-node manifests

- [ ] M120.P1.T1 — Per-node native-node registration fragments
  - files: `Engine/Nodes/<Family>/<Node>.reg.inc` (new, one per existing native node), `Engine/AppManager.cpp`, `Engine/CMakeLists.txt`, `Engine/Nodes/NativeRegistry.h` (new)
  - approach:
    - Each node gets a one-line fragment `NATRON_NATIVE_NODE(Class, "header.h")` beside its class. The registry reads ID, version, label, grouping, icon and the deprecated/internal flags from `Class`'s `NodeSpec` (M137 - Native Base Class Flexibility), so the fragment never repeats them. Icon files keep their current names.
    - Check the `NodeSpec` M137 actually merged first; if it already generates a registration line, the fragment is just that line.
    - CMake `file(GLOB_RECURSE ... CONFIGURE_DEPENDS Engine/Nodes/*.reg.inc)`, sorts the list, and generates `NativeRegistryAll.inc` (the `#include`s plus the macro lines), which `loadBuiltinNodePlugins()` includes in place of the hand lines at `AppManager.cpp:1631-1674`.
    - Migrate every native registration. Non-node built-ins (Roto, Tracker, Viewer, Precomp, DiskCache, JoinViews/OneView, Dot, Backdrop, Group) stay hand-registered.
    - Do **not** use static-initialiser self-registration: object files in the static engine library that nothing references get dropped by the linker, and their nodes would silently vanish.
  - verify: at the phase build, `NativePluginList_Test` and the full ctest pass, and the registered plug-in set is identical before and after. Diff the `getPluginIDs()` dump.
  - size: M

- [ ] M120.P1.T2 — Per-kernel GPU CMake fragments
  - files: `Engine/Gpu/CMakeLists.txt`, `Engine/Gpu/Kernels/<kernel>.cmake` (new, one beside each existing `<kernel>.slang`)
  - approach:
    - Move each `slang_add_kernel(...)` line from `Engine/Gpu/CMakeLists.txt` into a fragment named after its `.slang` file (a file with several entry points keeps all of its lines in one fragment).
    - The parent `file(GLOB ... CONFIGURE_DEPENDS Kernels/*.cmake)`s and `include()`s the fragments in sorted order, so the generated `GpuKernelId` enum is deterministic.
  - verify: at the phase build, the generated kernel registry lists the same IDs, and the GPU parity tests (`ctest -R Gpu`) pass on RADV.
  - size: M

- [ ] M120.P1.T3 — Per-node GPU cost seeds
  - files: `Engine/GpuCostModel.cpp`, `Engine/GpuCostSeed.inc` (split into `Engine/Nodes/<Family>/<Node>.costseed.inc` and `Engine/GpuCostSeeds/ofx/<plugin-id>.inc`), `tools/bench/render-profile-report.py`, `Engine/CMakeLists.txt`
  - approach:
    - Split the seed table into one fragment per node, beside the node. A CMake-generated aggregate (glob of `Engine/Nodes/*.costseed.inc`, sorted) replaces the include at `GpuCostModel.cpp:64`.
    - Rows for OFX plug-ins (Despill, FrameBlend, Retime, TimeBlur, Ramp, Radial, Rectangle…) go one per plug-in to `Engine/GpuCostSeeds/ofx/<plugin-id>.inc`, also globbed; a port milestone deletes the files for the IDs it retires, so no two lanes touch the same file.
    - Teach `--emit-cost-table` to write one fragment per node, finding the node's dir from its `.reg.inc`.
  - verify: at the phase build, `GpuCostModel` tests pass and the seeded cost for Grade/Blur/Despill is unchanged. A dry run of the emitter reproduces the split files byte-for-byte.
  - size: M

- [ ] M120.P1.T4 — Generated native-node icon resource
  - files: `Gui/GuiResources.qrc`, `Gui/CMakeLists.txt`, `cmake/` (a small qrc-generation helper if one isn't present)
  - approach:
    - Remove the `Resources/Images/NativeNodes/*.png` entries from `GuiResources.qrc`.
    - CMake globs `Gui/Resources/Images/NativeNodes/*.png` (CONFIGURE_DEPENDS) into a generated `NativeNodes.qrc` added to the Gui target, keeping the same resource paths, so `NATRON_IMAGES_PATH "NativeNodes/..."` lookups don't change.
  - verify: at the phase build, every native node shows its icon in the tab menu and the node graph (Xvfb screenshot of the tab menu's Color and Filter groups).
  - size: M

- [ ] M120.P1.T5 — Per-node docs and tolerance lines
  - files: `Engine/Nodes/README.md`, `Engine/Nodes/<Family>/docs/<Node>.md` (new, one per existing native node)
  - approach:
    - Move each node's notes and its row of the tolerance table (`README.md` around §658-676) into that node's doc file, as a `Tolerance:` line.
    - The top README keeps framework docs and says where per-node docs live (no hand index, so ports never edit it).
    - Pure text move. The policy rewrite is M111 - Native Node Identity Cleanup's.
  - verify: `git diff --stat` shows only moves, and every node section appears exactly once across the new files.
  - size: S

## Phase 120.2: Retirement as data, and the plug-in list test

- [ ] M120.P2.T1 — Host-side retired OFX ID markers
  - files: `Engine/OfxHost.cpp`, `Engine/OfxHost.h`, `Engine/OfxRetired/README.md` (new, explains the format), `Engine/CMakeLists.txt`, `Tests/OfxRetired_Test.cpp` (new)
  - approach:
    - Each retired OFX plug-in is one file, `Engine/OfxRetired/<ofx-plugin-id>`, with no extension, whose only content is an optional line naming the native ID that replaces it. One file per ID, so parallel lanes never edit the same file.
    - CMake `file(GLOB ... CONFIGURE_DEPENDS)`s the directory, ignores `README.md`, and generates a sorted `constexpr` array of IDs and replacements. `loadOFXPlugins()` skips any plug-in whose identifier is listed, next to the reader/writer skips (`OfxHost.cpp:946-953`).
    - The skip is a small predicate over that set. A test-only `OfxHost` hook adds extra IDs before the load, so no test marker ships.
    - Ship no markers. Each port milestone adds its own.
  - verify: at the phase build, `OfxRetired_Test` uses the hook to show a listed ID from the test bundle (Phase 120.3) is absent from `getPluginIDs()` while an unlisted one in the same bundle is present. It also checks that every shipped marker names an ID some loaded bundle provides; with no markers yet, that loop is empty.
  - size: M

- [ ] M120.P2.T2 — Derive the native plug-in list test from the registry
  - files: `Tests/Native/NativePluginList_Test.cpp`, `Tests/Native/NativePluginIdentity.h` (new: an `expectNativeIdentity(id, major, label)` helper for per-node tests), `Tests/CMakeLists.txt`
  - approach:
    - Replace the hand `nativeTable()` (`:80`): the test checks that every `.reg.inc` fragment's class is registered and every registered native ID comes from a fragment, both ways.
    - Move the per-node ID/major/label assertions into each existing node's own `Native<Node>_Test.cpp` through the new helper (mechanical; list the tests touched in the commit, splitting the commit if it is large). Port milestones do the same in their nodes' tests.
    - Keep `RetiredOfxIdsResolveToASingleNativeVersion`. Add checks that no two fragments register the same ID, that no retired ID is also a native ID or alias, and that each marker's replacement, when given, is registered.
  - verify: at the phase build, `ctest -R NativePluginList` passes and lists the same IDs as before.
  - size: M

- [ ] M120.P2.T3 — "Adding a native node" checklist
  - files: `Engine/Nodes/README.md`
  - approach: add a short section listing every file a port adds per node: the `.reg.inc`, the `.costseed.inc` (and deleting the `Engine/GpuCostSeeds/ofx/<plugin-id>.inc` rows it supersedes), `docs/<Node>.md` with its `Tolerance:` line, a `Native<Node>_Test.cpp` calling `expectNativeIdentity`, a `Kernels/<kernel>.cmake` per new `.slang`, the icon PNG, and one `Engine/OfxRetired/<ofx-id>` marker per OFX ID it retires. Say that `NativePluginList_Test` and `OfxRetired_Test` fail on a missing or inconsistent fragment. Take the file names from what P1 and P2 actually built.
  - verify: every path pattern in the checklist matches a real file for at least one existing node, or the marker README.
  - size: S

## Phase 120.3: Move test fixtures off to-be-retired IDs

T6 and T7 come first: they build the test-only OFX bundle. The other tasks then swap each OFX plug-in that is about to be retired for a test-bundle plug-in with the same traits, or for an in-tree C++ test effect (`Tests/CountingTestEffect.h`, `MultiplanarTestEffect.h`). They never swap to a shipped plug-in that stays OFX for now, since later ports would retire it too. Each test keeps what it proves: OFX hosting, channel sets, layer knobs or scheduling. Where a test really is about the node being retired, leave it for that node's port milestone and say so in the task's commit.

- [ ] M120.P3.T6 — Test-only OFX bundle scaffolding and routing fixtures
  - files: `Tests/OfxTestPlugins/CMakeLists.txt` (new), `Tests/OfxTestPlugins/natronTest/{CMakeLists.txt,PluginList.cpp,RoutingTestPlugins.cpp}` (new), `Tests/CMakeLists.txt`, `Tests/wmain.cpp`, `Tests/OfxTestPlugin_Test.cpp` (new)
  - approach:
    - `Tests/OfxTestPlugins/CMakeLists.txt` defines `natron_add_test_ofx_bundle(<name> SOURCES ...)`, which builds a `MODULE` library (`PREFIX ""`, `SUFFIX ".ofx"`) into `${CMAKE_BINARY_DIR}/Tests/TestPlugins/<name>.ofx.bundle/Contents/Linux-x86-64/` and makes `Tests` and `GuiTests` depend on it. It globs `*/CMakeLists.txt` and adds each subdirectory, so each bundle is its own directory. M140 - Native Metadata Utilities adds `natronTestMetadata/` (raw C API) without editing this file.
    - The `natronTest` bundle is built on the in-repo OFX Support layer (`libs/OpenFX/Support`). Its single `getPluginIDs` lives in `PluginList.cpp`, as the Support layer requires one per module.
    - `Tests/wmain.cpp` adds the bundle directory (compile definition `NATRON_TEST_OFX_PLUGIN_DIR`) to `OFX_PLUGIN_PATH` before `AppManager::load()`.
    - Plug-ins, IDs `fr.natron.test.*`: a two-input copy (stands in for Switch, KeyMix and CopyRectangle-style routing), a generator with a format and colour knob (stands in for Ramp, ColorWheel and ColorBars), and an RGBA point op with a matrix knob (stands in for ColorMatrix). Give one of them major version 2, for `PluginIdentity_Test`.
  - verify: at the phase build, `OfxTestPlugin_Test` finds each ID in `getPluginIDs()`, creates and renders each, and the bundle is absent from `build/assets/Plugins/` and the AppImage staging input.
  - size: M

- [ ] M120.P3.T7 — Multiplanar, transform, multi-frame and OCIO-config fixtures
  - files: `Tests/OfxTestPlugins/natronTest/{TraitTestPlugins.cpp (new),PluginList.cpp,CMakeLists.txt}`, `Tests/OfxTestPlugin_Test.cpp`
  - approach: add test-bundle plug-ins for the remaining fixture traits:
    - a multiplanar pass-through with layer choices (for the channel-set and layer-knob tests);
    - a transform with a matrix knob that returns a transform (stands in for CornerPin, IDistort and Card3D);
    - a multi-frame filter whose `getFramesNeeded` asks for a knob-set frame range (stands in for FrameBlend and TimeOffset);
    - a filter that declares the `ocioConfigFile` and `ocioWorkingSpace` params the project pushes (for the OCIO host tests).
  - verify: at the phase build, `OfxTestPlugin_Test` checks each trait: the layer choices list the input's layers, the transform is reported, the frames needed match the knob, and `Project::pushOCIOConfigToNode` sets the OCIO params.
  - size: M

- [ ] M120.P3.T1 — Channel-set and layer-knob tests
  - files: `Tests/DefaultChannelSet_Test.cpp`, `Tests/LayerKnobs_Test.cpp`, `Tests/LayerKnobsRender_Test.cpp`, `Tests/KnobLayerSelect_Test.cpp`, `Tests/ChannelSetRender_Test.cpp`
  - approach: replace Switch/KeyMix/AdjustRoD/HSVTool/Quantize/ColorMatrix/Ramp/Radial/Rectangle/ColorWheel/Noise/CImgPlasma/CornerPin/IDistort/STMap fixtures with test-bundle plug-ins with the same multiplanar or channel traits (the two-input copy for routing, the generator, the transform, the multiplanar pass-through). The KeyMix quad tests (`ChannelSetRender_Test.cpp:1286-1369`) move to M121 - Native Routing And Mix. Leave them marked there.
  - verify: at the phase build, the five tests pass, and grepping them for every table ID finds only the deferred KeyMix cases.
  - size: M

- [ ] M120.P3.T2 — Colour-view, metadata, serialization and identity tests
  - files: `Tests/ColorViewsRender_Test.cpp`, `Tests/ProjectSerialization_Test.cpp`, `Tests/PluginIdentity_Test.cpp`, `Tests/Native/EngineHooks_Test.cpp`
  - approach: same swap. `EngineHooks_Test` uses ColorMatrix as a generic OFX point op, so use the test point op. `PluginIdentity_Test:51` (HueCorrect major 2) moves to the major-2 test plug-in.
  - verify: at the phase build, the four tests pass and contain no table IDs.
  - size: M

- [ ] M120.P3.T3 — Scheduler, time and residency tests
  - files: `Tests/SchedulerEquivalence_Test.cpp`, `Tests/TimeVaryingLayers_Test.cpp`, `Tests/FrameGraphBuild_Test.cpp`, `Tests/GpuResidency_Test.cpp`, `Tests/GeneratorLayer_Test.cpp`
  - approach:
    - Switch plumbing → the test two-input copy.
    - FrameBlend/TimeOffset multi-frame fetches (`FrameGraphBuild_Test.cpp:514-548`) → the test multi-frame filter. Keep the pre-fetch cap assertion.
    - Ramp/ColorWheel generator fixtures → the test generator.
  - verify: at the phase build, the five tests pass with the same assertions and contain no table IDs.
  - size: M

- [ ] M120.P3.T4 — Render, OCIO, GL and smoke tests
  - files: `Tests/ShuffleRender_Test.cpp`, `Tests/RemoveLayersRender_Test.cpp`, `Tests/PersistentMessage_Test.cpp`, `Tests/GLScheduler_Test.cpp`, `tools/ci/smoke_test.py`
  - approach:
    - Switch fixtures → the test two-input copy.
    - `GLScheduler_Test`'s OCIOColorSpace (`:69,296`) stays, with a marker comment for M127 - Native OCIO Nodes; no GL-capable test plug-in is built.
    - `smoke_test.py` runs against the AppImage, which has no test bundle, so its bundle checks (`:303-305`) switch to shipped IDs not in the table: NoOp in Misc, CImgSmooth in CImg and an arena node. The ColorMatrix render check (`:688-690`) moves to ColorBars.
  - verify: at the phase build, the four C++ tests pass. `python3 tools/ci/smoke_test.py` passes against the phase AppImage.
  - size: M

- [ ] M120.P3.T5 — OCIO host tests
  - files: `Tests/ProjectOCIO_Test.cpp`, `Tests/ProjectOCIOPlugins_Test.cpp`, `Tests/Native/NativeReadColor_Test.cpp`
  - approach: these test the project config reaching OFX OCIO plug-ins. Split them:
    - Config-push assertions (`Project::pushOCIOConfigToNode`) target the test OCIO-config filter (P3.T7).
    - Assertions about the OCIO plug-ins' own behaviour stay, marked for M127 - Native OCIO Nodes to port to the native nodes.
  - verify: at the phase build, the three tests pass, and the remaining OCIO-plug-in references carry the M127 - Native OCIO Nodes marker comment.
  - size: M

- [ ] M120.P3.T8 — Metadata and metadata-bridge tests
  - files: `Tests/Metadata_Test.cpp`, `Tests/OfxMetadataBridge_Test.cpp`
  - approach: same swap for the non-metadata fixtures: ColorWheel → the test generator, and ColorMatrix (`OfxMetadataBridge_Test.cpp:76-77`) → the test point op. Leave the Contribute, Print and TimeCode example IDs alone; M140 - Native Metadata Utilities moves them to its own test plug-ins.
  - verify: at the phase build, both tests pass, and the only table IDs left in them are the three M140 - Native Metadata Utilities metadata IDs.
  - size: M

## Decisions

- 2026-10-20 — Claimed despite the file-overlap test (user override): M120 shares `Engine/AppManager.cpp` with M108 - Retire Engine GL Host Path (GPU host), `Engine/GpuCostModel.cpp` with M107 - GPU Viewer Hand-off, and `Tests/CMakeLists.txt` with the parked M64 - Tiled Rendering. The user accepted resolving those conflicts at merge, as for M137.
- 2026-10-19 — **Runs after M137 - Native Base Class Flexibility; fragments derive from `NodeSpec`** (user, M137 grill): registration is designed once. The fragment names only the class and header. This milestone gains a dep on M137.

- 2026-10-13 — Created during the native-port planning grill: the user chose to run the new port milestones in parallel with no board order, which only works if they share no manifest files.
- 2026-10-13 — Retirement is a host-side skip list, not a fork rebuild: one `.so` holds many plug-ins, and per-milestone data files keep lanes from contending on `OPENFX_MISC_REF`.
- 2026-10-13 — Fragments are per node, not per family: Filter, Generator, Time and Transform each have two or three sibling port milestones meant to run in parallel lanes, and a shared family file would fail §8a's file-overlap test between them.

- 2026-10-19 — **Runs before M111 - Native Node Identity Cleanup** (user, grill): M111 edits the manifests this milestone splits, and this one unblocks a dozen ports. M120 was added to M111's Deps.
- 2026-10-19 — **Plug-in groups, `smoke_test.py` ID lists and `fetch-assets.sh` stay shared** (user, grill): new groups are rare (only M127 - Native OCIO Nodes might add one), and the §8a overlap check serialises the odd case.
- 2026-10-19 — **Retire through a compiled-in skip list now, then purge the forks** (user, grill): the user asked why a retired plug-in leaves any trace. The bundles hold many plug-ins per `.so`, so until every port lands, the OFX twin can only be hidden. Deleting per port would make every lane bump the same fork ref. M145 - OFX Fork Plugin Purge deletes the plug-ins and the skip mechanism once the ports are done. The list is compiled in, never read at runtime, and maps nothing in old projects.
- 2026-10-19 — **One marker file per retired OFX ID, `Engine/OfxRetired/<ofx-id>`** (user, grill): it replaces the per-milestone `M<i>.txt`, so no plan ID appears in a code filename. It was chosen over listing replaced IDs in each node's `.reg.inc`.
- 2026-10-19 — **Fixtures move onto an in-repo test-only OFX bundle** (user, grill): it was chosen over shipped plug-ins that stay OFX for now, which later ports would retire too. M140 - Native Metadata Utilities adds its metadata plug-ins to the same bundle.
- 2026-10-19 — **Ports get a README checklist, enforced by the registry tests** (user, grill), rather than a scaffold script.
- 2026-10-19 — Settled without asking: the aggregates sort fragments by path, so `GpuKernelId` and the registration order are deterministic; icons come from a CMake-generated `NativeNodes.qrc`; tests add retired IDs through an `OfxHost` hook, so no test marker ships.

**Verification gate:**
- Full build and ctest pass on a clean build dir.
- `ctest -R Gpu` passes on RADV.
- The registered native set, kernel IDs and seeded costs are unchanged from before the milestone.
- `OfxRetired_Test` and `OfxTestPlugin_Test` pass, and the test bundle is absent from the AppImage.
- Grepping `Tests/` and `tools/ci/` for the table's IDs finds only cases explicitly deferred to their port milestone, and the M140 - Native Metadata Utilities metadata IDs.
- An Xvfb tab-menu screenshot shows the native icons.
