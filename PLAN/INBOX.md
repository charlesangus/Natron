## Pending

- **Input-data refresh scales badly on long chains** (PM, 2026-11-26, from M299 - Roto Render Performance): `AppManager::clearAllCaches` → `forceComputeInputDependentDataOnAllTrees` recurses about 80 frames of `refreshInputRelatedDataInternal` down a 70-item Merge chain, so `Operators/RotoPaintDirectCompositeTest` takes 9–38 s per case under -j4 (it calls `CacheStress::clearEverything` before every render). M299 runs that entry serially as a stopgap. Profile and make the refresh linear in the graph size; related to the O(N²) upstream walks in the 2026-09-25 render-scaling findings.

- **Shared geometry group selection** (user, 2026-11-25, raised during M600 - Native Geometry UAT Round 2): extract the Group / Group Type logic into one shared component used by every native geometry node, so no node reinvents it or differs from the others in how it presents or applies the rule. GeoUVProject only got a Group Type knob in M600. The component covers:
  - the knobs, labels, tooltips and Guess-from-Group resolution;
  - the rule that blank means every element of the class;
  - mapping a point or primitive selection onto points, vertices or primitives;
  - leaving unselected elements untouched.

  Audit every Geo* node that takes a Group (GeoTransform, GeoUVProject, GeoDisplace, GeoWrangle, …) and move each onto it, with one shared test. When in doubt, match Houdini. Feature: F15 follow-up, or a standalone milestone after F15 lands.

- **VDB grids in the geometry file cache** (PM, 2026-11-25, from the M417 - VDB Volume Primitive review): M403 - Geometry File Cache (in F18) does not serialize `GeoDetail::sharedGrids()`, but M417 (in F22) makes VDB primitives valid. When F18 and F22 meet, whichever lands second must add grid stream I/O to the GeoFileFormat writer and reader. Until then, a cached detail with VDB prims reloads with a dangling `vdbGrid` index and throws "VDB primitive has no grid". Add a round-trip test.

- **Layered document flatten: clipping and group isolation** (PM, 2026-11-25, from the M79 - Layered Document Readers review): when a PSD or KRA has no stored composite, the flattened composite ignores clipping masks and group isolation; only Normal-over is documented. PSD and Krita clip differently, so this is a design task.

- **For-Each loop thread scaling** (PM, 2026-11-25, from the M402 - Geometry Loops bench): 10,000 connectivity pieces cook in 1684 ms on 1 thread and 1404 ms on 4 (1.20x). Profile where the per-piece work serialises (piece extraction, merge, per-iteration setup or a lock), and re-bench on a many-core host.

- **User knob properties still not saved** (PM, 2026-11-25, from the F16 - User Knob Editor review): increments and decimals, Double matrix display, primPathHint, customHTMLText, and per-dimension min/max for Int/Double/Color (one range is saved for all dimensions). They are lost on save/load, copy/paste and Ctrl-drop copy. Parametric min/max is dropped by the copy too.

- **An expression reading another dimension of its own knob never updates** (PM, 2026-11-25, from F16 - User Knob Editor's gate): `size.h = thisNode.size.getValue(0)` keeps its first result when `size.w` changes, because self-reads register no listener (`Engine/PyParameter.cpp:1026` `_addAsDependencyOf`, `Engine/Knob.cpp:3310` `registerExpressionReads`). Pre-existing on main. A fix would register cross-dimension self-reads and keep refusing a dimension reading itself; check cycle handling in `refreshListenersAfterValueChange`.

- **SplitOnGapsTest stalls at startup** (PM, 2026-11-26): timed out at 300 s on f18-gate1 (passed on the serial rerun), and the watchdog dumped it "still running 225 s after the process started, phase 'start'" in m496-rep8's ctest leg. The stall is before the test body, so look at the watchdog stacks from rep8's log for what process start waits on (likely shared with other startup-phase stalls under -j4).
- **CyclesDelegate.DrawFilterHidesPrimsAndLights flaked once** (PM, 2026-11-26, m199-gate14): saw `/World/a` still visible after the draw filter hid it; passed on rerun. Watch whether it repeats on main or only on M199's branch (scheduler timing).

- **Pausing at a playback loop wrap leaves the view a frame behind** (PM, 2026-11-26, from M600 - Native Geometry UAT Round 2): when playback is paused just after the scheduler moves the timeline from the last frame back to the first, the first frame's evaluation is aborted, so the timeline shows frame 1 while the 3D view still shows the last frame; seeking to 1 is then a no-op (`TimeLineGui::seek` returns early when the time is unchanged). A pause should leave the view on the timeline's frame (re-render the current frame after an abort).

### 2026-11-27 — change-request — USDToGeo detail-attribute promotion
- from: bosley-executor (cat-pm)
- refs: M611
- body: M611 - USDToGeo Source Controls keeps constant primvars as detail attributes (the audit's "Detail Attributes promotion — Out for now"). Review of PR #406 found constant instancer primvars then don't reach instance points. Proposed follow-up: the reference set's Promote to Primitive if Different / Primitive / Point / Vertex control on USDToGeo, applied to unpacked prims and point instances alike so they agree.

### 2026-11-27 — change-request — USDToGeo info message opens a dialog
- from: bosley-executor (cat-pm)
- refs: M418
- body: `USDToGeo.cpp` (~:426) posts its info through an info-type persistent message, which is never stored on the node: on a render thread it only reaches the error log, on the main thread it opens a modal dialog that can block a re-render. M418 - VDB Conversion Ops fixed the same problem in VDBFromPolygons/VDBFromParticles by using the node's quiet info message. Small fix: do the same in USDToGeo (and audit other nodes for info-type persistent messages).

### 2026-11-27 — change-request — Delegate render tests flake under ctest load
- from: bosley-executor (cat-pm)
- refs: M602
- body: On M602 - 3D Viewer Toolbars gate10 (run 46043776540, fast preset), `USDRenderCyclesMaterialTest` segfaulted on the first attempt and `USDRenderMoonRaySubdivisionTest` timed out on the rerun; neither touches M602's code, and the F36/M609 runs earlier saw `ProgressiveRenderTest` segfault the same way. Also on PR #404 CI (run 46044117803): NatronUtilitySamplingDelegate segfault, ProgressiveRenderTest failure and USDRenderMoonRayLiveTextureTest timeout in one run, all passing on ctest retry. These now cost a ~90 min rerun on most PRs. Proposed milestone "Delegate Test Flakes": reproduce under parallel ctest (shuffle seeds), find the shared cause (delegate teardown, GPU/engine reuse across tests, or timeouts sized for an idle runner) and make them deterministic.
