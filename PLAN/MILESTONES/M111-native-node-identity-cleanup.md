# M111 - Native Node Identity Cleanup

Native nodes stop pretending to be OFX plug-ins:
- **Plain IDs:** every native node gets a plain ID (`Grade`, `Merge`, `Shuffle`, `DeepMerge`, `Read`, …) at major version 1. Reverse-domain IDs are for OFX plug-ins running in an arbitrary host, not for nodes built into Natron.
- **Siblings become one node:**
  - Transform and TransformMasked → `Transform`
  - Erode and Dilate → `ErodeDilate`
  - Constant and Solid → `Constant`
- **Removed:** the duplicate menu entries, the `PLUGINID_OFX_*` aliases, the Natron 1.x/2.x loaders, and the dead compat knobs.
- **Read and Write** drop openfx-io's knob names.

There is no compatibility: old projects and scripts that use the old IDs load the missing-plugin node, per `DECISIONS/2026-09-18-clean-break-no-project-compat.md`. User decisions 2026-10-12: `DECISIONS/2026-10-12-native-nodes-drop-ofx-legacy.md`.

Deps: M101 - Clean-Sheet Merge Node (it builds the base class's multi-input support that Dissolve needs here, and it already moves Merge, RotoPaint's merges and the PyPlugs' merges to the plain `Merge` ID) and M76 - Native Write (Phase 107.5 renames its knobs). Also M120 - Native Node Port Kit (added 2026-10-19): it splits the registration, README and plug-in-list manifests this milestone edits. At promotion, rewrite the briefs against its per-node `.reg.inc`, `docs/<Node>.md` and identity tests, and rename the OFX-ID-named icons in `Gui/Resources/Images/NativeNodes/` along with the IDs.

Notes from M113 - Native Warp Nodes (2026-10-24), which shipped the first plain-ID native nodes (`Mirror`, `STMap`, `IDistort`, `CornerPin`). `Engine/CMakeLists.txt` now validates an OfxRetired marker's replacement with a plain-or-dotted regex (the marker file name stays a dotted OFX ID). Check these during the mass rename:
- The node-menu tree (`Gui/GuiApplicationManagerPrivate.cpp:146-160`) keeps groups and plug-ins in one list per level, matched by `getID()`. A plain ID equal to a sibling subgroup name under the same parent would be taken for that group; dotted IDs made that impossible.
- `Engine/AppManager.cpp:2251-2259` `getPluginBinaryFromOldID` maps `Viewer`, `Dot`, `DiskCache`, `Backdrop`. It only runs after an exact lookup fails, but the renames should check it.
- `Engine/Nodes/README.md:971-990` ("IDs and versions") still says native nodes keep the OFX ID at a higher major.

Behaviour and maths fixes are in M112 - Native Node Behaviour Fixes. The `hidpi`/`interactive` knobs and the keyers are in M95 - Knob Cleanup And Keyer Redesign.

Scouted facts (2026-10-12, read-only audit on `milestone/m45-native-tab-menu`; re-check line numbers at promotion):
- **OFX-era IDs:**
  - 28 native nodes still use them, defined as `PLUGINID_NATRON_*` macros in each node header (e.g. `Engine/Nodes/Color/Grade.h:34`).
  - Their majors are "OFX major + 1" (e.g. `Filter/Blur.h:40`, `Filter/EdgeDetect.h:47`). No OFX twin is left, and `NativePluginListTest.RetiredOfxIdsResolveToASingleNativeVersion` (`Tests/Native/NativePluginList_Test.cpp:127`) enforces that.
  - `AppManager::getPluginBinary` (`AppManager.cpp:2346`) falls back from the requested major to the next one up, then to the highest.
  - Read is `fr.inria.built-in.Read`. Shuffle, ShuffleCopy, AddLayers, RemoveLayers, the 11 Deep* nodes and TypedPassthrough are `fr.natron.*`.
- **Sibling registrations:** `AppManager.cpp:1621-1658`.
  - TransformMasked: `Transform/Transform.h:43`, `Transform.cpp:210`, with its own mask × mix loop at `Transform.cpp:1166-1196`.
  - Erode/Dilate: `Filter/ErodeDilate.h:42-45, 91-95`. The tooltip says size "&gt;=0" but the minimum is -1000 (`ErodeDilate.cpp:196-198`).
  - Constant/Solid: `Generator/Constant.h:34-35, 75`.
  - Add/Multiply/Gamma stay separate nodes (user). They may keep sharing a class.
- **`PLUGINID_OFX_{MERGE,TRANSFORM,GRADE,COLORCORRECT,COLORLOOKUP,BLURCIMG,CONSTANT}`** (`EffectInstance.h:50-65`) duplicate the native macros and are used by Roto, Tracker and the GUI. COLORLOOKUP has no users.
- **Legacy loaders:**
  - The 1.x/2.x knob-name and choice-option filters: `KnobSerialization.cpp:447-659`, applied at `Node.cpp:1762`.
  - The pre-1.0 "label [group]" ID remap, plus `"RotoOFX  [Draw]"`: `AppManager.cpp:2236-2311`, called from `AppInstance.cpp:1147`.
- **Dead or OFX-only knobs** (M95 already removes `hidpi`/`interactive`):
  - Transform: `srcClipChanged` (`Transform.cpp:513`), `transformInteractOpen` (`:383`), and the hidden `uniform` (`:288-297, 555-568`).
  - Transform: `transformCenterChanged` (`:373`) is used at `:580` and set by the AngleBlur, Glow and DropShadow PyPlugs.
  - ColorLookup: `hasBackgroundInteract` (`ColorLookup.cpp:494-500`, which hides the ramp/histogram display at `:681-695`) and `showRamp` (`:551-557`), which is never read.
  - Crop: `frameRange` "kept for parity only" (`Crop.cpp:287`).
  - Reformat: the `scaleUniform_separator` knob (`Reformat.cpp:422-427`).
- **Sublabels:** Shuffle, AddLayers, RemoveLayers, DeepRemoveLayers and Keyer set them through `kNatronOfxParamStringSublabelName` (`Channel/Shuffle.cpp:231` and others). The LightWrap and EdgeBlur PyPlugs set it too.
- **Dissolve:**
  - The mask sits at index 2 among 64 inputs (`Dissolve.h:38-42, 77`).
  - It has its own `maskInvert` and no mix knob (`Dissolve.cpp:266-271, 625-651`).
  - With an input unconnected, it uses the project extent from the origin (`:360-365`).
- **NativeRead:**
  - It keeps openfx-io's knob names: `before`, `after`, `frameMode`, `startingTime`, `timeOffset`, `originalFrameRange`, `proxyThreshold`, `customProxyScale` (`IO/NativeRead.cpp:214-231`).
  - It keeps the GenericOCIO pattern of `ocioInputSpace` + `ocioInputSpaceIndex` + a hidden `ocioInputSpaceSet`, plus `Context` (`NativeRead.h:45`, `NativeRead.cpp:229-232, 525-530`). `WriteNode.cpp:812` and `Project.cpp:304` depend on it.
- **PyPlugs** call `createNode` with OFX IDs and explicit majors, e.g. `("net.sf.cimg.CImgBlur", 3)`. `Tests/Native/PyPlugNativeParams_Test.cpp` pins their knob names.
- **Tests:** `Tests/NativeParity.cpp:626` still has `ParityPair.ofx`/`ofxMajor` plumbing.

## Phase 107.1: Policy

- [ ] M111.P1.T1 — Rewrite the native-node policies in the README and simplify the parity harness
  - files: `Engine/Nodes/README.md`, `Tests/NativeParity.{h,cpp}`
  - approach:
    - Replace the "Host knobs and script names" OFX-copy rule (`README.md:~551-554`), "IDs and versions" (`~605`), "Siblings are one class under several IDs" (`~615-621`) and the "deliberate fixes" list with the new rules:
      - plain IDs at major 1
      - one node per feature, with Merge's operation presets as the only menu-entry presets
      - knobs, defaults and maths designed on their own merits, camelCase script names
      - parity fixtures are regression fixtures, re-recorded whenever behaviour changes on purpose
    - Keep the tolerance classes, re-titled as regression tolerances.
    - Strip the `ParityPair.ofx`/`ofxMajor` plumbing so fixtures replay against a recorded reference only.
  - verify: the README contains no instruction to match an OFX plug-in. The parity tests still pass at the gate.
  - size: M

## Phase 107.2: Plain IDs

- [ ] M111.P2.T1 — Give every native node a plain ID at major 1
  - files: every native node header that defines a `PLUGINID_NATRON_*`/major macro under `Engine/Nodes/**`, plus `Engine/AppManager.cpp` (registration) and `Engine/Nodes/IO/NativeRead.h`. This is a mechanical change over many headers, kept as one coherent task.
  - approach:
    - Set each macro to the node's plain display name, without spaces (`Grade`, `ColorCorrect`, `Blur`, `EdgeDetect`, `Shuffle`, `ShuffleCopy`, `AddLayers`, `DeepMerge`, `Read`, `Write`, …), and every native major to 1.
    - Erode/Dilate, Constant/Solid and TransformMasked are handled in Phase 107.3.
    - Keep the macro names, so callers compile unchanged.
    - Check that no plain ID collides with a bundled OFX plug-in's ID.
    - Update `NativePluginList_Test` to expect plain IDs and no reverse-domain native ID.
  - verify: `NativePluginList_Test` lists every native node by its plain ID at major 1. `git grep -E '"(net\.sf\.(openfx|cimg)|eu\.cimg|fr\.natron|fr\.inria\.built-in)\.'` finds only OFX-plugin references (openfx bundles, OFX tests).
  - size: M

- [ ] M111.P2.T2 — Remove the `PLUGINID_OFX_*` aliases and point their callers at the native macros
  - files: `Engine/EffectInstance.h`, `Engine/Roto*.cpp` (those still using them after M101), Tracker and Gui callers found by `git grep PLUGINID_OFX_`
  - approach: replace each `PLUGINID_OFX_X` use with `PLUGINID_NATRON_X`, delete the alias block, and set knobs by their native names. Merge's callers were already moved by M101.
  - verify: `git grep PLUGINID_OFX_` is empty. Roto and Tracker tests pass at the gate.
  - size: M

- [ ] M111.P2.T3 — Move the bundled PyPlugs to plain IDs
  - files: `Gui/Resources/PyPlugs/*.py`, `Tests/Native/PyPlugNativeParams_Test.cpp`
  - approach:
    - Replace every native node's ID and major in `createNode` with the plain ID at 1.
    - Replace `transformCenterChanged` writes if P4.T2 makes it internal, and the sublabel writes per P2.T5.
    - Update the pinned names in `PyPlugNativeParams_Test`.
  - verify: every bundled PyPlug instantiates and renders a fixture at the gate. `PyPlugNativeParams_Test` passes.
  - size: M

- [ ] M111.P2.T4 — Point the tests at the plain IDs
  - files: the `Tests/**` files found by `git grep` for the old native IDs; if there are more than about 10, split them by directory in the execution report
  - approach: a mechanical ID swap. Where a test asserted an OFX-era major or a version fallback, rewrite it for major 1.
  - verify: the full ctest passes at the gate.
  - size: M

- [ ] M111.P2.T5 — Add a native sublabel hook
  - files: `Engine/EffectInstance.h` (or `NativeEffectBase.h`), the host code that reads `kNatronOfxParamStringSublabelName`, `Engine/Nodes/Channel/Shuffle.cpp`, `AddLayers.cpp`, `RemoveLayers.cpp`, `Engine/Nodes/Deep/DeepRemoveLayers.cpp`
  - approach: add a `virtual std::string getSublabel() const` that the node graph reads for native nodes. Native nodes stop creating the hidden OFX sublabel knob. OFX plug-ins keep the knob path. Keyer is moved by whichever of M95 or this task lands second.
  - verify: the sublabels show in the graph (GUI check at UAT). No native node declares `kNatronOfxParamStringSublabelName`.
  - size: M

## Phase 107.3: One node per feature

- [ ] M111.P3.T1 — Fold TransformMasked into Transform
  - files: `Engine/Nodes/Transform/Transform.{h,cpp}`, `Engine/AppManager.cpp`, `Tests/Native/NativeTransform_Test.cpp`
  - approach:
    - Transform gains an optional mask input and uses the base class's mask/mix. Delete its own loop at `Transform.cpp:1166-1196` and the TransformMasked ID.
    - There is no TransformMasked menu entry.
  - verify: tests cover Transform with and without a mask, mask invert, and mix. `NativePluginList_Test` has no TransformMasked.
  - size: M

- [ ] M111.P3.T2 — Fold Erode and Dilate into ErodeDilate
  - files: `Engine/Nodes/Filter/ErodeDilate.{h,cpp}`, `Engine/AppManager.cpp`, `Tests/Native/NativeErodeDilate_Test.cpp`
  - approach: one ID, `ErodeDilate`. A signed size: positive erodes and negative dilates, as in Nuke's FilterErode. The tooltip must state the range correctly. There are no Erode or Dilate menu entries.
  - verify: tests for erode, dilate and size 0 identity.
  - size: M

- [ ] M111.P3.T3 — Fold Solid into Constant
  - files: `Engine/Nodes/Generator/Constant.{h,cpp}`, `Engine/AppManager.cpp`, `Tests/Native/NativeConstant_Test.cpp`, any PyPlug that creates Solid
  - approach: delete the Solid ID and its menu entry. Constant is unchanged.
  - verify: `NativeConstant_Test` passes. No Solid in the plug-in list.
  - size: S

- [ ] M111.P3.T4 — Put Dissolve on the base class's mask/mix path
  - files: `Engine/Nodes/Merge/Dissolve.{h,cpp}`, `Tests/Native/NativeDissolve_Test.cpp`
  - approach:
    - Use M101's multi-input point-op support and `addMaskMixKnobs`. Delete the own `maskInvert` knob and the mask blend at `Dissolve.cpp:625-651`, and gain mix.
    - Mask is the last input, and source inputs are 0..n-1.
    - With an input unconnected, the region of definition is the union of the connected inputs, not the project extent from the origin.
    - Re-check the "mask region misses → pass-through of the lower input" identity rule (`:398-412`) against the actual blend.
  - verify: tests for mask, invert and mix, an unconnected input's region, and `which` animation.
  - size: M

## Phase 107.4: Dead code

- [ ] M111.P4.T1 — Delete the Natron 1.x/2.x project loaders
  - files: `Engine/KnobSerialization.cpp` (`:447-659` filters), `Engine/Node.cpp` (`:1762` call), `Engine/AppManager.cpp` (`getPluginBinaryFromOldID`, `:2236-2311`), `Engine/AppInstance.cpp` (`:1147`)
  - approach: delete the filters and the old-ID remap with their callers, per the clean-break decision. Keep anything M93 - Serialization Boundary's golden-XML harness still needs. Check M93's state first.
  - verify: the build and the full ctest pass at the gate. `git grep getPluginBinaryFromOldID` is empty.
  - size: M

- [ ] M111.P4.T2 — Delete the dead compat knobs
  - files: `Engine/Nodes/Transform/Transform.cpp`, `Engine/Nodes/Color/ColorLookup.cpp`, `Engine/Nodes/Transform/Crop.cpp`, `Engine/Nodes/Transform/Reformat.cpp`
  - approach:
    - Delete `srcClipChanged`, `transformInteractOpen`, Transform's hidden `uniform` (keep the scale knob's fold/lock), ColorLookup's `showRamp`, Crop's `frameRange` and Reformat's `scaleUniform_separator`.
    - Replace ColorLookup's `hasBackgroundInteract` with unsaved node state and show the ramp/histogram controls.
    - Make `transformCenterChanged` internal state if P2.T3 can drop it from the PyPlugs. Otherwise keep it and say so in the report.
  - verify: the node tests pass. The knob lists in the UAT shots have none of these knobs.
  - size: M

## Phase 107.5: Read and Write knob names

- [ ] M111.P5.T1 — Give Read and Write native knob names and one colourspace knob
  - files: `Engine/Nodes/IO/NativeRead.{h,cpp}`, `Engine/Nodes/IO/NativeWrite.{h,cpp}`, `Engine/Project.cpp` (`:304`), `Engine/WriteNode.cpp` (`:812`, if it still exists after M76), the Read/Write tests that pin knob names
  - approach:
    - Rename the openfx-io knob names to camelCase native names that describe the behaviour (e.g. `before` → `beforeFirstFrame`, `frameMode` + `startingTime` + `timeOffset` → a clear frame-offset pair). Decide the final names in the task and list them in the report.
    - Replace `ocioInputSpace` + `ocioInputSpaceIndex` + `ocioInputSpaceSet` + `Context` with one colourspace knob (and Write's output equivalent), keeping project OCIO behaviour (M50 - Project OCIO Support).
    - Update the gui test scripts `Tests/gui/m75_read.py` and `m76_write.py`.
  - verify: the Read/Write test suites pass at the gate. The GUI scripts pass under Xvfb.
  - size: L

## Phase 107.6: Close-out

- [ ] M111.P6.T1 — UAT
  - files: none (user acceptance)
  - approach: hand the user a build and a checklist:
    - the tab menu shows one entry per node (Merge presets excepted)
    - node IDs in the script editor and in a saved `.ntp` are plain
    - Transform with a mask
    - ErodeDilate both ways
    - Dissolve with mask and mix
    - Read/Write knob names and the colourspace knob
    - every bundled PyPlug renders
    - an old project shows missing-plugin nodes, not a crash
  - verify: the user signs off. Record the findings under `## Decisions`.
  - size: S

**Verification gate:**
- One build. Full ctest green in both scheduler modes, and CI green.
- `git grep` finds no reverse-domain native ID and no `PLUGINID_OFX_`, `getPluginBinaryFromOldID` or `TransformMasked`.
- Every bundled PyPlug renders.
- The user signs off the UAT.
