## Pending

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
