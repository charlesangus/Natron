# M93 - Serialization Boundary

Full title: Serialization boundary (plain-data snapshot layer, no format change)

**Runs first, starting now** (user, 2026-10-10). M16 - Project Format Redesign follows it in
this file and depends on it.

Refactor the Engine and Gui serialization so archive shape no longer depends on Engine classes.
Every `*Serialization` type becomes plain data in a new standalone `Serialization/` static
library (`NatronSerialization`): no `KnobIPtr`/`NodePtr`, and no boost outside one adapter
directory. Engine and Gui classes produce and consume that data through
`toSerialization`/`fromSerialization`. The boost XML archive becomes non-intrusive adapters in
`Serialization/BoostXml/` that read and write **the same bytes as today**. The `.ntp` format
and behaviour do not change. M16 then deletes `Serialization/BoostXml/` wholesale.

Sequencing: M93 does not wait for M76 - Native Write or M92 - Linked Nodes. It migrates
`Engine/WriteNode.cpp`'s in-memory XML stash like any other consumer (P4.T4). When M76 merges,
M93 rebases onto it and drops that hunk, since M76 deletes the file. M93 does no M92 work: it
carries the master-link fields (`MasterNode`, `MasterNodeName`/`MasterKnobName`/`MasterDimension`)
into the plain structs unchanged, and M92 later edits those structs.

Scouting facts (2026-10-10, `main` at `6b0ec1f42`):
- About 8.2k lines across `Engine/*Serialization.{h,cpp}` (25 files) and
  `Gui/{NodeGui,NodeBackdrop,ProjectGui}Serialization.{h,cpp}`. Version ladders are at node
  v17, knob v15 and value v7.
- The save side reads live knobs while the archive runs (`Engine/KnobSerialization.h:547-626`,
  `ValueSerialization` at `:294-357`). The load side creates throwaway knobs
  (`KnobSerialization::createKnob`, `Engine/KnobSerialization.cpp:~150-180`) that
  `Node::loadKnob` (`Engine/Node.cpp:1722-1858`) then copies from.
- Intrusive `serialize()` exists on `Curve`, `KeyFrame`, `BezierCP` (3 templates), `Format`,
  `RectI`/`RectD`, `Variant`, `ImageLayerDesc`, and the disk-cache keys
  (`FrameKey`, `ImageKey`, `ImageParams`, `FrameParams`, `NonKeyParams`).
- GUI state goes through `AppInstance::saveProjectGui/loadProjectGui(boost archive&)`
  (`Engine/AppInstance.h:217-221`; `Gui/GuiAppInstance.cpp:784`, `Gui/Gui20.cpp:1398`,
  `Gui/ProjectGui.cpp:267,435`).
- 61 Engine/Gui files outside the serialization files use `NodeSerialization` or
  `KnobSerialization` as an in-memory snapshot.

## Phase 93.1: Harness, library and knob data

- [ ] M93.P1.T1 — Golden-XML byte-for-byte regression harness (before anything moves)
  - files: new `Tests/Serialization/GoldenXml_Test.cpp`, new `Tests/fixtures/golden-ntp/*.ntp`, new `Tests/fixtures/golden-ntp/README.md` (how to regenerate), `Tests/CMakeLists.txt` (add a `Serialization/*_Test.cpp` glob beside the `Native/` one at `:114`)
  - approach: the inputs are each of `Tests/fixtures/*.ntp` and `tools/bench/comps/*.ntp`, loaded, plus three graphs built in code: a generator→Grade→Write with an animated knob and an expression; a group with a user knob and a nested group; and a RotoPaint with a stroke and an animated feathered bezier plus a Tracker with 2 tracks. Save each, then **normalize** by stripping the values of `CreationDate`, the save-date and last-author knobs, the version/git/OS/bits fields, `KnobsAge` and `CacheID`. Compare the result byte-for-byte with a committed golden produced by the *current* code. `NATRON_REGEN_GOLDEN=1` rewrites the goldens. Commit the goldens before P1.T3 starts. This harness is the refactor's safety net; M16 deletes it along with the XML format.
  - verify: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `ctest -R GoldenXml` in the devshell. It passes on unmodified code and fails when one knob default is changed by hand (spot-check, then revert). The build is required.
  - size: M

- [ ] M93.P1.T2 — `Serialization/` library skeleton and include rule
  - files: new `Serialization/CMakeLists.txt`, new `Serialization/SerializationFwd.h`, new `Serialization/README.md`, `CMakeLists.txt`, `Engine/CMakeLists.txt`
  - approach: a static library `NatronSerialization` that depends on the std library only. `Serialization/BoostXml/` alone may include boost.serialization. Engine links it PUBLIC. The README states the rules: plain data only, no Engine/Gui/Qt/Python includes, conversion lives in Engine and Gui. Add the check to `lint-ci` (or a CMake configure-time grep): any `Serialization/**` file that includes `Engine/` or `Gui/` fails.
  - verify: compiled at P1.T6. A deliberately bad include, tried locally and not committed, makes the lint check fail.
  - size: M

- [ ] M93.P1.T3 — Plain curve, keyframe, format, rect and variant data
  - files: new `Serialization/CurveSerialization.h/.cpp`, new `Serialization/BasicSerialization.h` (Format, RectI/RectD, Variant data), new `Serialization/BoostXml/BasicTypesXml.h`, `Engine/Curve.h/.cpp` (with `Engine/CurveSerialization.h/.cpp` removed)
  - approach: `CurveSerialization{ keys: vector<KeyFrameSerialization{time, value, interp, leftDeriv, rightDeriv}> }`, plus `Curve::toSerialization` and `fromSerialization`. Write the boost adapters as non-intrusive free `serialize(Archive&, T&, unsigned)` functions with the **same NVP names and class versions** (`BOOST_CLASS_VERSION` on the data types). Remove the intrusive `Curve::serialize`. `RectI`/`RectD`/`Format` keep their intrusive `serialize` only because the cache TOC (`Engine/FrameKey.h`, `ImageKey.h`, `ImageParams.h`) needs it; leave a one-line comment saying so.
  - verify: compiled at P1.T6, where GoldenXml stays byte-identical.
  - size: M

- [ ] M93.P1.T4 — Knob data types, XML adapter and Engine conversions (additive)
  - files: new `Serialization/KnobSerialization.h/.cpp`, new `Serialization/BoostXml/KnobXml.h`, new `Engine/KnobSerializationConvert.cpp` (with its declarations in `Engine/Knob.h`), new `Tests/Serialization/KnobXmlEquivalence_Test.cpp`
  - approach: `KnobSerialization` holds:
    - name, type name, dimension, and flags (secret, enabled per dimension);
    - a per-dimension value: either a `variant<bool,int,double,string>` or a `CurveSerialization`;
    - expression and hasRet; the master/link fields, unchanged for M92; choice ID and label;
    - typed extra data (choice entries and helps, value min/max/display min/max, file sequences, path multi, layer-select channel buttons, text flags);
    - user-knob metadata, parametric curves and string animation.

    `KnobHelper::toSerialization` moves the archive-time reads into Engine and applies `hasModificationsForSerialization` and the channel-set always-save rule. `fromSerialization` applies the data to an existing knob, and `createUserKnobFromSerialization` creates user knobs from their type name. The XML adapter reproduces today's NVP sequence and the knob v15 and value v7 ladders, including reading older versions. This task only *adds* code; nothing calls it yet.
  - verify: compiled at P1.T6. For every knob type in `createKnob`'s list (including `KnobChannelSet`, `KnobLayerSelect` and `KnobShuffleMap`), in static, animated, expression, linked and user-knob variants, the gtest checks two things. The old `KnobSerialization(knob)` XML and the new `toSerialization` → `KnobXml` XML are byte-identical. Loading that XML through the new path and calling `fromSerialization` reproduces the knob state.
  - size: M

- [ ] M93.P1.T5 — Switch the Engine to knob data and delete the old knob serialization
  - files: `Engine/KnobSerialization.h/.cpp` (removed), `Engine/Node.cpp` (`loadKnob`, `:1722-1858`, plus the knob loop at `:1631`), `Engine/NodeSerialization.cpp` (knob collection, `:100-120`), `Engine/Knob.cpp`, `Engine/KnobTypes.cpp`
  - approach: `NodeSerialization` (still the old class at this point) holds `Serialization::KnobSerialization` values and archives them through `KnobXml`. `Node::loadKnob` consumes the data. Delete `createKnob` and the throwaway-knob path. Move the in-memory boost round-trips in `Tests/KnobChannelSet_Test.cpp:917`, `Tests/KnobLayerSelect_Test.cpp:430,465,592` and `Tests/KnobShuffleMap_Test.cpp` onto the data type plus `KnobXml`.
  - verify: compiled at P1.T6, where GoldenXml stays byte-identical and the knob tests pass.
  - size: L

- [ ] M93.P1.T6 — Phase 93.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green, with GoldenXml byte-identical. The build is required.
  - size: S

## Phase 93.2: Roto and tracker data

- [ ] M93.P2.T1 — Roto data types, XML adapter and Engine conversions (additive)
  - files: new `Serialization/RotoSerialization.h/.cpp` (data for BezierCP, Bezier, RotoStrokeItem, RotoDrawableItem, RotoLayer, RotoItem and RotoContext), new `Serialization/BoostXml/RotoXml.h`, new `Engine/RotoSerializationConvert.cpp`, new `Tests/Serialization/RotoXmlEquivalence_Test.cpp`
  - approach: same pattern as P1.T4. The legacy quirks stay in the adapter only: the RC2/RC3 `curveBug` branch (`Engine/BezierCPSerialization.h:100-110`) and `OffsetTime`.
  - verify: compiled at P2.T4. A gtest checks old-path and new-path XML are byte-identical for a stroke, an animated feathered bezier with a broken tangent, and nested layers, and that a load through the new path reproduces them.
  - size: M

- [ ] M93.P2.T2 — Switch roto to the data model and delete its old serialization headers
  - files: `Engine/BezierCP.h/.cpp`, `Engine/Bezier.cpp`, `Engine/RotoStrokeItem.cpp`, `Engine/RotoDrawableItem.cpp`, `Engine/RotoLayer.cpp`, `Engine/RotoContext.cpp`; `Engine/{BezierCP,Bezier,RotoContext,RotoDrawableItem,RotoItem,RotoLayer,RotoStrokeItem}Serialization.h` are removed
  - approach: mechanical but wide (6 edited files plus header deletions; accepted as one coherent change). Replace the intrusive `BezierCP::serialize` and its `_imp` access with the P2.T1 conversions. `NodeSerialization` holds `RotoContextSerialization` data.
  - verify: compiled at P2.T4, where GoldenXml's roto graph stays byte-identical and `Tests/RotoLayer_Test.cpp` passes.
  - size: L

- [ ] M93.P2.T3 — Tracker data model
  - files: new `Serialization/TrackerSerialization.h/.cpp`, new `Serialization/BoostXml/TrackerXml.h`, `Engine/TrackMarker.cpp`, `Engine/TrackerContext.cpp`, `Engine/TrackerContextPrivate.h`; `Engine/TrackerSerialization.h` is removed
  - approach: `TrackSerialization{enabled, isPM, label, scriptName, knobs: vector<KnobSerialization>, userKeys}` and `TrackerContextSerialization{tracks}`, built on P1.T4, with conversions done in place.
  - verify: compiled at P2.T4, where GoldenXml's tracker graph stays byte-identical and `Tests/TrackerLayer_Test.cpp` passes.
  - size: M

- [ ] M93.P2.T4 — Phase 93.2 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green, with GoldenXml byte-identical. The build is required.
  - size: S

## Phase 93.3: Node, project and GUI data

- [ ] M93.P3.T1 — Node, collection and project data types, XML adapter and conversions (additive)
  - files: new `Serialization/NodeSerialization.h/.cpp`, new `Serialization/ProjectSerialization.h/.cpp`, new `Serialization/BoostXml/ProjectXml.h`, new `Engine/ProjectSerializationConvert.cpp`, new `Tests/Serialization/ProjectXmlEquivalence_Test.cpp`
  - approach: `NodeSerialization` holds no `NodePtr`. It holds:
    - plugin ID, major/minor, label, script name, python module and version, knobs;
    - `inputs` (label→name) and the legacy index-based `oldInputs`;
    - master node name (unchanged, for M92);
    - roto and tracker data, multi-instance parent, user pages, page order, children;
    - the read-only `version` it was loaded from.

    `ProjectSerialization` drops its `AppInstancePtr`. It holds project knobs, additional formats, layer-registry entries (layer data plus origin), timeline, node counters and loaded-version info. Conversions: `Node::toSerialization`, and `ProjectPrivate` builds from and restores the data.
  - verify: compiled at P3.T5. A gtest checks old-path and new-path XML are byte-identical for the three P1.T1 built graphs.
  - size: M

- [ ] M93.P3.T2 — Switch node and project load/save to the data model
  - files: `Engine/NodeSerialization.h/.cpp` and `Engine/ProjectSerialization.h/.cpp` (removed), `Engine/NodeGroupSerialization.h/.cpp` (`restoreFromSerialization` takes the data and keeps its algorithm), `Engine/ProjectPrivate.cpp`, `Engine/Project.cpp`
  - approach: `saveProjectInternal` and `loadProjectInternal` run the archive through `BoostXml/ProjectXml.h`. The version-gated Engine reads (`Node.cpp:1636,1759`, `ProjectPrivate.cpp:273`) read `version` from the data.
  - verify: compiled at P3.T5, where GoldenXml stays byte-identical and `ProjectSerialization_Test`, `DefaultChannelSet_Test` and the layer and OCIO project tests pass.
  - size: L

- [ ] M93.P3.T3 — GUI data types, XML adapter and conversions (additive)
  - files: new `Serialization/ProjectGuiSerialization.h/.cpp` (NodeGui, Backdrop, ViewerData, Histogram, PaneLayout, GuiLayout), new `Serialization/BoostXml/ProjectGuiXml.h`, new `Gui/ProjectGuiSerializationConvert.cpp`, new `Tests/Serialization/ProjectGuiXmlEquivalence_Test.cpp` (in GuiTests if it needs a Gui)
  - approach: same pattern as P1.T4, covering `Gui/ProjectGuiSerialization.h:596-722`'s fields and the version ladder.
  - verify: compiled at P3.T5. Old-path and new-path XML are byte-identical for a populated GUI state, mirroring `Tests/ViewerDataSerialization_Test.cpp`.
  - size: M

- [ ] M93.P3.T4 — Make the `AppInstance` GUI hook boost-free
  - files: `Engine/AppInstance.h`, `Gui/GuiAppInstance.h/.cpp`, `Gui/Gui.h`, `Gui/Gui20.cpp`, `Gui/ProjectGui.h/.cpp`; `Gui/{NodeGui,NodeBackdrop,ProjectGui}Serialization.*` are removed
  - approach: `saveProjectGui(ProjectGuiSerialization*)` and `loadProjectGui(bool isAutosave, const ProjectGuiSerialization&)` replace the archive overloads. `Project` runs the archive. Remove `<boost/archive/...>` from `Engine/AppInstance.h`. Move `Tests/ViewerDataSerialization_Test.cpp` onto the data type.
  - verify: compiled at P3.T5, where GoldenXml stays byte-identical.
  - size: L

- [ ] M93.P3.T5 — Phase 93.3 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green, with GoldenXml byte-identical. The build is required.
  - size: S

## Phase 93.4: In-memory consumers

- [ ] M93.P4.T1 — Undo, clipboard and node-graph consumers
  - files: `Gui/KnobUndoCommand.cpp`, `Gui/NodeGraphUndoRedo.h/.cpp`, `Gui/NodeGraph40.cpp`, `Gui/NodeClipBoard.h`, `Gui/NodeGraph35.cpp`/`NodeGraph45.cpp`/`NodeGraphPrivate*.cpp` (only the call sites)
  - approach: switch to the data types and conversions; behaviour does not change. The clipboard keeps XML text through `BoostXml` until M16 replaces it.
  - verify: compiled at P4.T5.
  - size: M

- [ ] M93.P4.T2 — Node creation, PyPlug, preset and precomp consumers
  - files: `Engine/AppInstance.h/.cpp` (`onGroupCreationFinished`), `Engine/CreateNodeArgs.h`, `Engine/NodeMain.cpp`, `Engine/PrecompNode.cpp`, `Gui/NodeSettingsPanel.cpp`, `Gui/NodeGui.cpp`
  - approach: as P4.T1.
  - verify: compiled at P4.T5, where `PyPlugExport_Test` passes.
  - size: M

- [ ] M93.P4.T3 — Knob-dialog consumers in Gui
  - files: `Gui/EditExpressionDialog.cpp`, `Gui/EditScriptDialog.cpp`, `Gui/LinkToKnobDialog.cpp`, `Gui/AddKnobDialog.cpp`, `Gui/DotGui.cpp`, `Gui/KnobGuiPrivate.h`
  - approach: as P4.T1.
  - verify: compiled at P4.T5.
  - size: M

- [ ] M93.P4.T4 — Remaining Engine consumers, including the WriteNode stash
  - files: `Engine/PyParameter.cpp`, `Engine/OfxEffectInstance.cpp/.h`, `Engine/OfxHost.cpp`, `Engine/NodeName.cpp`, `Engine/WriteNode.cpp/.h` (`:203,381,429-513`)
  - approach: as P4.T1. In `WriteNode`, keep the knob stash in memory as data instead of an XML string. If M76 has already merged at rebase time, drop this hunk because M76 deletes the file. Finish with `grep -rln "boost/archive" Engine Gui`, which should list only `Engine/Project.cpp`, the cache files (`AppManagerPrivate.cpp`, cache key headers), the `.nl` layout sites (`Gui/Gui10.cpp`, `Gui/Gui15.cpp`) and the clipboard and preset sites.
  - verify: compiled at P4.T5. Record the grep output in the task notes.
  - size: M

- [ ] M93.P4.T5 — Phase 93.4 build, full ctest and smoke
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green, with GoldenXml byte-identical. The build is required.
  - size: S

## Phase 93.5: Close-out

- [ ] M93.P5.T1 — AppImage smoke UAT of the GUI-only serialization paths
  - files: `build/appimages/M93-<sha>.AppImage`, `build/appimages/M93-uat.md`
  - approach: build with the release `tools/ci/local/package.sh` and launch-check it with `build/appimages/run-launch-check.sh`. The checklist:
    - open, save and reopen each `tools/bench/comps/*.ntp`;
    - undo and redo a knob change and a node delete;
    - copy and paste nodes;
    - export and import a preset;
    - save and load a `.nl` layout;
    - open a project with a RotoPaint and a Tracker saved by the pre-M93 build;
    - a headless `NatronRenderer` render of a comp.
  - verify: the user signs off. **Stop for the user.**
  - size: M

**Verification gate:** GoldenXml byte-identical on every fixture and built graph at every phase
checkpoint. Full ctest, smoke and CI (`format`, `lint-ci`, `build-and-test`) green. No
`Serialization/**` file includes Engine or Gui (lint). `Engine/AppInstance.h` no longer
includes boost archives. The user signs off the UAT.

## Decisions

- 2026-10-10 — **User decision:** M93 is approved as a separate milestone that precedes M16, and it starts now. It does not wait for M76 or M92. It rebases onto M76 when M76 merges, dropping the WriteNode hunk. M92 builds on M93's plain structs; M93 does no M92 work and carries the master-link fields unchanged.
- 2026-10-10 — **User decision:** keep the golden-XML byte-for-byte harness as M93's safety net, even though M16 retires the XML format afterwards.
- 2026-10-10 — The boost adapters live only in `Serialization/BoostXml/` (named for what they are: the current format, not a compat layer). They keep today's NVP names and class versions, so M16 can delete the directory without touching Engine.
- 2026-10-10 — Each data type lands in two steps: first the types, adapter and conversions, with an old-versus-new byte-equivalence gtest (purely additive); then the Engine switch and deletion of the old class. Each verify then stands on its own.
- 2026-10-10 — RB-3 is used as a reference only. M93 adopts its architecture (a plain-data `Serialization/` library plus Engine `toSerialization`/`fromSerialization`). It does not port `KnobTableItemSerialization` (RB-3's roto/tracker model, absent here) or `SerializationCompat` (not needed under the clean break).
- 2026-10-10 — Out of scope: the disk-cache TOC's boost binary archive and the intrusive `serialize()` on cache keys.
- 2026-10-10 — Build deferral (user: slow host, defer builds, half the cores): M93.P1.T1 and M93.P1.T2 are implemented first and verified by one shared cold build that also generates the goldens; each is still committed separately. Every build uses `NATRON_BUILD_JOBS=4`.
