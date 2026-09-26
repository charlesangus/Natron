# Milestone 37: Channel/layer management nodes

> **Draft (2026-09-26): the consultant's elaboration, written for its recommended answers to Q1–Q6 below. Awaiting the user's answers; do not start implementation until they are recorded under `## Decisions`.**

Two native nodes in the Channel group. **Remove** takes layers out of the stream, or keeps only the chosen ones. **AddLayers** brings project-registry layers into the stream, zero-filled, wherever the input lacks them. Both choose layers with M38's channel set, including its Regex rows, resolved per frame at the render's (time, view) (`DECISIONS/2026-09-24-layers-vary-with-time.md`).

Neither node moves data between layers, so the no-shuffle invariant (`PLAN/DESIGN/2026-09-19-layer-channel-widget.md` §5) stands:
- Remove renders no pixels. Its kept layers are forwarded from the input by the existing pass-through path (`EffectInstanceRenderRoI.cpp` ~462-501).
- AddLayers writes zeros only into layers its input doesn't carry.

The engine gains one capability virtual: a per-node filter on pass-through layers. Today pass-through is "every input layer the node doesn't produce", with no per-layer opt-out (`EffectInstance.cpp` ~4610-4621). Image data only; deep is M60.

## Design questions (awaiting the user)

- **Q1. Is Remove one node or two?** (a) One `Remove` node with an operation choice, remove or keep; the default is remove, with nothing selected. (b) Two nodes, Remove and Keep. (c) Remove only. *Recommend (a).*
- **Q2. Remove single channels, or whole layers only?** (a) Whole layers only: no channel buttons, and the no-shuffle rule is unchanged. (b) Single channels too, on non-colour layers; this amends the no-shuffle rule and replaces P1.T2 with an L task in P2. (c) Channel removal inside the colour plane too, through the rgba/rgb/alpha views (see Q3(b)); +2 L tasks. *Recommend (a); (b) can follow later.*
- **Q3. Can the colour views (rgba/rgb/alpha/xy) be removed?** (Reworded after M65.) (a) No: they are never listed or matched, and keep always keeps the colour plane. (b) Removing `alpha` narrows the colour plane to RGB, and removing `rgb` narrows it to Alpha. Since M65 makes missing colour channels read zero, this is close to zero-filling. (c) Removing `rgba` drops the colour plane entirely; +1 M task. *Recommend (a).*
- **Q4. Wildcard, regex, or both?** (a) Reuse M38's Regex rows. (b) Add a Wildcard mode to `KnobChannelSet` for every node; +1 M engine task, +1 M GUI task. (c) Wildcard on Remove and AddLayers only. *Recommend (a). The risk is people typing globs such as `spec*` as a regex.*
- **Q5. Is an Add node needed, and what does it do?** (a) `AddLayers`: zero-fills the chosen registry layers only where the input lacks them, leaves present layers untouched, and can add several at once. (b) As (a), plus a fill-colour knob. (c) No node: document the Shuffle and Constant recipes instead. *Recommend (a).*
- **Q6. Names?** (a) `fr.natron.Remove` "Remove" and `fr.natron.AddLayers` "AddLayers", both in Channel. (b) RemoveLayers and AddLayers. (c) Remove and AddChannels, as in Nuke. *Recommend (a).*
- **Default taken, not asked:** both nodes resolve their selection per frame at the render's (time, view). A row naming a layer the input doesn't have is silent: it shows "(not in input)" and doesn't fail the render, unlike Shuffle's explicit rows.

Execution notes:
- Stacked on M65: branch off `milestone/m65-rgba-rgb-alpha-layers` and open the PR against it (`DECISIONS/2026-09-22-stacked-milestone-prs.md`).
- **After M65 (2026-09-26):** "Color" is gone from the user's view. It is replaced by the colour views `rgba`/`rgb`/`alpha`/`xy`, which share one storage colour plane and are always present, with missing colour channels reading zero (`PLAN/DESIGN/2026-09-26-rgba-rgb-alpha-layers.md`). Every "Color" in this file's briefs must be re-read in those terms, and the §5a freshness check at promotion rewrites them:
  - engine assertions (`getPresentLayers`) stay at storage level, i.e. `kNatronColorLayerID`;
  - user-facing lists expect the views, e.g. `{rgba, rgb, alpha, diffuse, specular}`;
  - `listsColor` becomes `listsColorViews`, built on `listLayerViewsForKnob`/`expandColorViews`;
  - "the input lacks Color" in P3.T1 becomes "the input has no colour plane".
- The `natron-dev` container is single-tenant. Implementers edit in parallel and don't build. Each batch gets one detached build plus ctest (setsid+nohup, a fresh `.done` marker). Check `pgrep -x ninja` is 0 before relaunching, and never pgrep-wait on a build.
- Run tests through `build/m61ctest.sh <regex>` (it sets `OFX_PLUGIN_PATH`) or `tools/ci/local/test.sh`.
- The debug build defines NDEBUG, so tests use EXPECT/ASSERT, never assert().
- GUI checks run under Xvfb with the recipe in `build/deeprepro/run-gui.sh` (see also `build/m61-gui/run-gui.sh`). Scripts and fixtures go under `build/m37-gui/`.
- Package the release build for UAT. Launch-check with the devshell `LD_LIBRARY_PATH` stripped (`build/appimages/run-launch-check.sh`).
- Batches:
  - B1: P1.T1, P1.T2, P1.T3
  - B2: P2.T1, P4.T1
  - B3: P2.T2, P3.T1, P4.T2
  - B4: P3.T2, P5.T1
  - B5: P4.T3, P6.T1, P6.T2
- `AppManager.cpp` and `Tests/CMakeLists.txt` each get a one-line edit from both P2.T1 and P3.T1, so those two tasks run in different batches.

## Phase 37.1: Engine foundations

- [ ] M37.P1.T1 — Add a per-node filter on pass-through layers
  - files: `Engine/EffectInstance.h` (~1082-1099), `Engine/EffectInstance.cpp` (~4432-4453, ~4610-4621)
  - approach: add the virtual `filterPassThroughLayers(double time, ViewIdx view, std::list<ImageLayerDesc>* layers)`, a no-op by default, documented next to `isPassThroughForNonRenderedLayers`. Call it with the pass-through (time, view) at two points: in the multiplanar branch of `getComponentsNeededAndProduced_public` after `removeFromLayersList`, and at the end of `getComponentsNeededDefault`'s pass-through fill. Don't call it in the disabled branch (~4543-4568). The filtered list is what the ActionsCache stores, so `getPresentLayers` and render forwarding see it with no further change.
  - verify: full debug ctest green. There's no behaviour change yet: P2.T1 is the first node to override it and carries the behaviour tests.
  - size: M

- [ ] M37.P1.T2 — Let a channel set hide its channel buttons (Q2)
  - files: `Engine/KnobChannelSet.h` (~118-254), `Engine/KnobChannelSet.cpp`, `Engine/PyParameter.cpp`, `Tests/KnobChannelSet_Test.cpp`
  - approach: add `setWithChannelButtons(bool)` / `getWithChannelButtons()` as a non-persistent member, default true (precedent: `KnobLayerSelect.h` ~103-116). With the buttons off:
    - `setChannels`/`setExcludedChannels` throw `std::invalid_argument`, which Python sees as `ValueError`;
    - `resolve()` ignores the stored channels and yields every channel of each matched layer.
    The codec is unchanged.
  - verify: `ctest -R KnobChannelSet`:
    - with buttons off, a `diffuse` row that stores `{R}` resolves to all three bits;
    - `setChannels` throws;
    - a regex row with an exclusion resolves to every channel;
    - the default (buttons on) is unchanged.
  - size: M

- [ ] M37.P1.T3 — Let a declared layer knob leave Color out of its listing (Q3)
  - files: `Engine/NodePrivate.h` (~79-110), `Engine/Node.h` (~1468-1475), `Engine/Node.cpp` (~5783-5838, ~5914-5922), `Tests/LayerKnobs_Test.cpp`
  - approach: `LayerKnobSource` gains `listsColor`, default true, and `declareLayerKnob` gains a matching defaulted parameter. When it is false, `listLayersForKnob` drops Color from input-bound lists (skipping the always-list-Color rule at ~5826-5836) and from target lists. References and the knob's role are unchanged.
  - verify: gtest with a user channel set on a Blur fed by Read(`flat-three-layers.exr`):
    - declared input-bound with `listsColor=false`, it lists `{diffuse, specular}`;
    - with the default, it lists `{Color, diffuse, specular}`;
    - declared as a target with `listsColor=false`, the list has no Color.
  - size: M

## Phase 37.2: Remove

- [ ] M37.P2.T1 — Remove node: keep/remove over a channel set, no pixels rendered (Q1–Q4, Q6)
  - files: `Engine/Nodes/Channel/Remove.h`, `Engine/Nodes/Channel/Remove.cpp` (new), `Engine/AppManager.cpp` (~1563), `Tests/Remove_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - Plugin: `NativeEffectBase`, `fr.natron.Remove`, label "Remove", `PLUGIN_GROUP_CHANNEL`, one optional input "Source". Multiplanar; `producesMetadataLayerImplicitly` returns false (precedent: `Shuffle.h` ~91-99). All bit depths.
    - Knobs:
      - `operation`: `KnobChoice` {remove, keep}, default remove, not animated, metadata slave.
      - `channels`: `KnobChannelSet`, buttons off (P1.T2), default None, metadata slave, not animated, declared input-bound on input 0 with `listsColor=false` (P1.T3).
      - A sublabel such as "remove diffuse, specular" (see `Shuffle.cpp` ~216-223).
    - `getComponentsNeededAndProduced`: produced is empty, inputs empty, pass-through is input 0 at (time, view).
    - `filterPassThroughLayers`: resolve `channels` against the incoming layers minus Color. Remove drops the matched layers; keep retains the matched layers plus Color.
    - `isIdentity`: onto input 0 when nothing would be dropped at (time, view).
    - `render`: a no-op returning `eStatusOK`.
    - Register the node next to Shuffle.
  - verify: `ctest -R Remove_` on Read(`flat-three-layers.exr`) → Remove:
    - the node is registered in Channel, and a new node is identity;
    - `getPresentLayers(1,0,-1)` is `{Color, specular}` when removing `diffuse`, `{Color}` when removing regex `.*`, `{Color, specular}` when keeping `spec.*`, and `{Color}` when keeping None;
    - a downstream Blur's lists follow knob changes;
    - a row naming an absent `depth` posts no error;
    - `removeLayer("diffuse")` is refused while Remove names it.
  - size: L

- [ ] M37.P2.T2 — Remove renders and varies per frame
  - files: `Tests/RemoveRender_Test.cpp` (new; model it on `ShuffleRender_Test.cpp` ~126-152, ~171-496), `Tests/CMakeLists.txt`
  - approach: Read → Remove → Write (All, single-part 32f).
    - Removing `diffuse` writes exactly `R,G,B,A,specular.*` with the fixture values; keeping `spec.*` writes the same.
    - Remove followed by a Grade (All) renders cleanly.
    - Time: reuse the sequence and Switch builders (~241-330) with remove regex `diff.*`. Frame 1 has no `diffuse.*`; frame 2 is RGBA with the fixture values. Render each frame with the timeline parked on the other frame.
  - verify: `ctest -R RemoveRender` green; full debug ctest green.
  - size: M

## Phase 37.3: AddLayers

- [ ] M37.P3.T1 — AddLayers node: zero-fill registry layers the input lacks (Q5, Q6)
  - files: `Engine/Nodes/Channel/AddLayers.h`, `Engine/Nodes/Channel/AddLayers.cpp` (new), `Engine/AppManager.cpp` (~1564), `Tests/AddLayers_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach:
    - Plugin: `fr.natron.AddLayers`, label "AddLayers", Channel group, optional input "Source". Multiplanar; `producesMetadataLayerImplicitly` returns false.
    - Knob `layers`: `KnobChannelSet`, buttons off, default None, metadata slave, declared as a target (lists the registry).
    - Produced: the resolved registry layers that input 0 lacks at (time, view). Color is included only if the input lacks it. Pass-through is input 0.
    - `isIdentity` when nothing would be produced.
    - `render`: `Image::fillZero(roi)` on each output plane (`Engine/Image.h` ~700).
    - Region of definition: the Source's, or the project format when unconnected.
    - Sublabel such as "diffuse, mask".
  - verify: `ctest -R AddLayers_` with `mask [A]` registered, on Read(`flat-three-layers.exr`) with rows `mask` and `diffuse`:
    - produced is `{mask}`, and present is `{Color, diffuse, specular, mask}`;
    - an unconnected AddLayers(`mask`) presents `{mask}`;
    - `removeLayer("mask")` is refused while the node names it;
    - a new node is identity.
  - size: L

- [ ] M37.P3.T2 — AddLayers renders zeros, never overwrites, and varies per frame
  - files: `Tests/AddLayersRender_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Read(`flat-three-layers.exr`) → AddLayers(`mask`, `diffuse`) → Write(All).
    - `mask.A` is 0, and `diffuse` stays `(0,1,0)`, untouched.
    - On the sequence with row `diffuse`: frame 1's diffuse is `(0,1,0)` and frame 2's is zero. Render each frame with the timeline parked on the other frame.
    - A downstream Grade can select `mask`.
  - verify: `ctest -R AddLayersRender` green; full debug ctest green.
  - size: M

## Phase 37.4: GUI

- [ ] M37.P4.T1 — Channel-set rows without channel buttons (Q2)
  - files: `Gui/LayerChannelRow.h`, `Gui/LayerChannelRow.cpp`, `Gui/KnobGuiChannelSet.cpp`, `Tests/LayerChannelRow_Test.cpp`
  - approach: `LayerChannelRow` set rows take `withChannelButtons`. When it is false there is no button strip on layer rows and no button line under regex rows (the `matches:` label stays). `KnobGuiChannelSet` passes `knob->getWithChannelButtons()`.
  - verify: GuiTests (offscreen): with buttons off, layer rows and matching regex rows have no buttons; with buttons on, nothing changes.
  - size: M

- [ ] M37.P4.T2 — "New layer…" on channel-set rows of a target knob (Q5)
  - files: `Gui/LayerChannelRow.cpp` (~584-590), `Gui/KnobGuiChannelSet.h`, `Gui/KnobGuiChannelSet.cpp` (~254), `Gui/KnobGuiLayerSelect.h`, `Tests/LayerChannelRow_Test.cpp`
  - approach: set rows list "New layer…" when the knob is a target: pass `isTargetKnob()` at ~254. Declare `runNewLayerDialog` (`KnobGuiLayerSelect.cpp` ~191) in the header. `KnobGuiChannelSet` handles `newLayerRequested` the way `KnobGuiLayerSelect::onNewLayerRequested` does (~168-189): deferred via `QTimer::singleShot(0, …)`, in one undo step.
  - verify: GuiTests: a target set row lists "New layer..." last; an input-bound set row doesn't. The dialog flow is covered by P4.T3.
  - size: M

- [ ] M37.P4.T3 — Xvfb screenshots of both panels, the viewer menu and the node graph
  - files: `build/m37-gui/gui.py`, `build/m37-gui/run.sh`
  - approach: build Read(`flat-three-layers.exr`) → Remove → AddLayers → Viewer and screenshot:
    - the Remove panel: operation, a `diffuse` row, a regex row with `matches:`, no buttons, no Color;
    - the viewer's layer menu before and after Remove;
    - AddLayers "New layer…" → `mask [A]`, its panel, and Project Settings → Layers showing `mask` used by 1;
    - the node graph with both sublabels.
  - verify: send the shots to the user; the task stays open until the user approves them.
  - size: M

## Phase 37.5: Round trip and scripting

- [ ] M37.P5.T1 — Save/load and PyPlug export for Remove and AddLayers
  - files: `Tests/Remove_Test.cpp`, `Tests/AddLayers_Test.cpp`, `Tests/PyPlugExport_Test.cpp`
  - approach:
    - Save, reset and load a project containing Remove(keep, `spec.*`) and AddLayers(`mask`). Rows, operation and present layers survive.
    - Export a group containing AddLayers on `mask [A]`. The script has one `addProjectLayer("mask", ["A"])` before the node is created, and reimporting it registers `mask`.
    - In Python, `setChannels` on either knob raises `ValueError`.
  - verify: `ctest -R 'Remove_|AddLayers_|PyPlugExport'` green.
  - size: M

## Phase 37.6: Checkpoint

- [ ] M37.P6.T1 — Publish the M37 decision
  - files: `docs/decisions/2026-09-2x-remove-and-add-layers.md` (new), its `PLAN/DECISIONS/` mirror, `PLAN/DECISIONS/INDEX.md`
  - approach: record the Q1–Q6 answers and the pass-through filter hook, in the style of `2026-09-24-layers-vary-with-time.md`.
  - verify: the file exists and INDEX links to it.
  - size: S

- [ ] M37.P6.T2 — Packaged release AppImage and user checkpoint
  - files: `build/appimages/M37-<sha>.AppImage`, `build/appimages/M37-uat.md`
  - approach: build with the release `package.sh`. The UAT script walks through:
    - Remove `diffuse`: the viewer menu and a Write-All EXR no longer have it;
    - keep `spec.*`, with Color never offered;
    - regex matches updating when the Read's file changes;
    - AddLayers "New layer…" `mask`, then a Grade selecting `mask`;
    - AddLayers on a `diffuse` that's already present leaves it untouched;
    - scrubbing the sequence 1↔2;
    - undo;
    - removing a layer from the Layers page is refused while a node names it.
  - verify: the AppImage launches under Xvfb via `run-launch-check.sh`; the user runs the script and signs off.
  - size: S

**Verification gate:**
- `tools/ci/local/test.sh ctest debug` and `smoke debug` are green. That includes KnobChannelSet, LayerKnobs, Remove, RemoveRender, AddLayers, AddLayersRender, PyPlugExport, TimeVaryingLayers and ShuffleRender, plus GuiTests LayerChannelRow.
- The user has approved the P4.T3 screenshots.
- The user has signed off the P6.T2 UAT.
- The decision is published.

## Decisions
