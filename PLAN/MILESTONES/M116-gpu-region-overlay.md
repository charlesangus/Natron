# M116 - GPU Region Overlay

Make the scheduler's GPU placement visible in the node graph, so users can see how the comp is being optimized and restructure it to keep work resident on the GPU. A subtle outline wraps the nodes and pipes of each resident region that M84 - GPU Placement And Residency's placement pass formed (`FrameGraph::Task::regionId` and `FrameGraph::Region`, `Engine/RenderScheduler.h`; `placeGraph` in `Engine/GpuPlacement.cpp`). Every pipe where a region ends gets a red X, with short text beside it that says why the region stopped there. The overlay shows the regions of the frame the active viewer rendered most recently, and updates after each viewer render (user, 2026-10-13).

Today `placeGraph` drops or splits candidates without recording why, and nothing passes placement to the GUI. This milestone adds both:
- an end reason for each region exit, recorded where the placement pass decides it;
- the last viewer render's placement, published to the GUI.

It reuses the precedent of the node graph's Show Expressions toggle: a View-menu action with a node-graph shortcut (`kShortcutIDActionGraphShowExpressions`, `Gui/NodeGraph35.cpp:445`, `Gui/GuiApplicationManager10.cpp:925`). The difference is that this toggle also persists as a preference.

The overlay shows every region exit with a red X and reason, entry pipes with a neutral tick, cached members faded, a distinct outline for a runtime fallback, and "mixed" where merged graphs disagree. A preference on the Node Graph page (default off), a node-graph shortcut (Shift+G unless taken) and a View-menu action share one state. With GPU compute off or no GPU, enabling it draws nothing and raises no error. The shortcut precedent is ShowLinkLines: action `Gui/NodeGraph35.cpp:445`, slot `toggleKnobLinksVisible` `:292`, keybind dispatch `Gui/NodeGraph25.cpp:430`, registration `Gui/GuiApplicationManager10.cpp:943`, IDs `Gui/ActionShortcuts.h:521`, preferences `Engine/Settings.cpp` ~1431, and `Settings::setAutoTurboModeEnabled` used at `NodeGraph25.cpp:511` for an action writing a preference.

## Scope

**In:**
- Recording a reason for each region exit in the placement pass, and in the region body for runtime fallbacks. Reasons are a fixed category enum recorded in `placeGraph` (no GPU path, cost model, would form a cycle, raw run all-or-nothing, downscale unsupported, …). `canRenderOnGpu` returns an optional short detail string with the category (e.g. "mask input", "5 channels").
- Keeping `inputNb` on `FrameGraph` dependency edges, so each marker lands on the exact pipe. `buildGraph` drops it today, and a node wired to two inputs of one consumer records only one edge.
- A shadow placement with `isCached` forced false, run beside the real one. The overlay draws the shadow plan. Members whose output came from the cache in the real render are drawn faded.
- A dry-run planner for the active viewer's current frame and view: the request pass, `buildGraph` and `placeRenderGraph`, with no execution. It runs when the overlay is turned on and after any graph change (topology or knob), and the result of the next real viewer render replaces it.
- Publishing the last viewer render's placement (both placements, plus each region's runtime `RegionReport` fallback) to the GUI, only while the overlay is on. Recording the reason enum is always on.
- The outline, the red-X markers and their labels on exit pipes, and a neutral upload tick with no text on pipes entering a region.
- A runtime-fallback style for a region that placement put on the GPU but that ran on the CPU: a distinct outline (e.g. dashed or amber) labelled with the runtime reason.
- The preference (Node Graph page, default off), the shortcut and the View-menu action, all bound to one state.

**Out:**
- Changing placement decisions or the cost model. This milestone only reports them.
- Per-node GPU timings or transfer sizes in the graph. Profiles already carry them (M84 - GPU Placement And Residency).
- Showing a node's reason anywhere except on its region's exit pipes: no node tooltip and no CPU badge (none).
- Placement for Write or other non-viewer renders (none).
- Kernel fusion (M87 - Point-Op Kernel Fusion, cancelled).

**Edge cases:**
- A region feeding the viewer: the viewer is the natural end, so it gets no X. M107 - GPU Viewer Hand-off keeps the root on the device.
- A region crossing a Group node's boundary: in the parent graph, outline the Group node itself. Inside the group, outline its members. Groups are flattened in the `FrameGraph`, so interior tasks map to the enclosing Group's `NodeGui`.
- Several viewers: track the active viewer.
- Placement that differs between views or frames: show only what the last viewer render or dry run placed.
- One render that builds several graphs (one per split RoI rect, one per A/B input): merge them. A node placed differently across graphs is drawn as GPU with a small "mixed" tag.
- A region that fell back at runtime, or a frame that lost the device and re-ran on the CPU: draw the planned outline in the fallback style with the runtime reason. Fallback status maps onto the members of the shadow plan.
- A dry-run plan carries no runtime-fallback information. The fallback style appears only after a real render.
- A viewer render that never reaches the scheduler (paint stroke, tracking, force refresh, deep input, legacy mode): keep the last placement.
- A graph change after a render: re-plan with a dry run and swap the result in, with no fade or hide in between.
- Playback: freeze the overlay while playing, then update once playback stops.
- A pipe between two adjacent GPU regions (a split for a cycle or the cost model): a red X with the split reason.
- GPU compute off, no GPU, or Legacy scheduler mode: enabling the overlay draws nothing and does not error.

**Assumptions:**
- The default shortcut is chosen at elaboration, after checking for conflicts in the node-graph shortcut group.
- A dry run (request pass plus placement, no pixels) is cheap enough to run after every graph change while the overlay is on. If it is not, debounce it at elaboration.
- The GUI bench rule applies only if M117 - GUI Perf Harness And Baseline has landed by the time this milestone's gate runs.

## Phase 116.1: Engine reasons

- [x] M116.P1.T1 — Reason category enum plus optional detail string
  - files: `Engine/GpuPlacement.h`, `Engine/GpuPlacement.cpp`, `Tests/GpuPlacement_Test.cpp`
  - approach: Record a reason on each silent exit of `placeGraph` (~355-520): not `canRenderOnGpu`, `dropIncompleteRawRuns`, `tryAssign` cycle refusal, the regionGain shrink loop (`kMaxRegionShrinkSteps`) and identity. Extend `canRenderOnGpu` (currently `bool(Task&)`) with a detail out-param, feeding `makeGpuTaskPlanes`'s existing `refusal` string (~811) into it. Don't change any placement decision.
  - verify: one new `TEST(GpuPlacement, ...)` per category, using the fake `PlacementInputs`.
  - size: L
- [x] M116.P1.T2 — Keep `inputNb` on graph edges
  - files: `Engine/RenderScheduler.h`, `Engine/RenderScheduler.cpp`, `Tests/FrameGraphBuild_Test.cpp`
  - approach: `buildGraph` (~494-555) drops `TaskEdge::inputNb` (`ParallelRenderArgs.h:314`). Add a parallel `dependencyInputNbs` vector kept in sync with `dependencies` and `consumerTasks`, including the second copy at ~711-716, the region unit graph.
  - verify: a diamond graph and a graph with duplicate edges keep their input numbers.
  - size: M
- [x] M116.P1.T3 — Shadow plan with `isCached` forced false
  - files: `Engine/GpuPlacement.cpp`, `Engine/GpuPlacement.h`, `Tests/GpuPlacement_Test.cpp`
  - approach: Run `placeGraph` a second time on a copy of the inputs with `isCached` false, returning both placements and marking which members the real plan serves from cache. M276 - Upstream Reuse On Edits changes the key that lambda probes, so override it from outside rather than editing it.
  - verify: a cached member is CPU in the real plan and GPU in the shadow plan.
  - size: M
- [x] M116.P1.T4 — Capture the runtime fallback per region
  - files: `Engine/RenderScheduler.cpp` (`executeRegion` ~1265-1361), `Engine/RenderScheduler.h` (`RegionReport` ~242-350), `Tests/GpuRegionFallback_Test.cpp`
  - approach: `executeRegion` writes `cpuFallback`, `deviceLost` and `retries` only to the profile sink. Also keep them on the frame result, mapped from the `kFallback*` strings set in `runGpuRegion` (`GpuRegionTask.cpp` ~3395-3535).
  - verify: each `kFallback*` cause surfaces on the result.
  - size: M

## Phase 116.2: Publish and plan

- [x] M116.P2.T1 — Per-viewer snapshot
  - files: `Engine/ViewerInstance.h`, `Engine/ViewerInstance.cpp`, `Engine/ViewerInstancePrivate.h`, `Tests/Native/ViewerRootOnDevice_Test.cpp`
  - approach: Build the snapshot from both placements plus the fallbacks, under its own lock, in `submitViewerFrameToRenderScheduler` (:1301-1309), only while the overlay is on. Merge the split-rect graphs (submitted at ~2558) and the A/B calls into one snapshot, tagging disagreement "mixed". Ignore the Write path at `OutputSchedulerThread.cpp:2594`.
  - verify: with the overlay off, no shadow plan and no snapshot; split rects that disagree produce "mixed".
  - size: L
- [x] M116.P2.T2 — Dry-run planner
  - files: `Engine/ViewerInstance.cpp`, `Engine/ViewerInstance.h`, `Tests/Native/ViewerRootOnDevice_Test.cpp`
  - approach: Re-plan after every graph change using ParallelRenderArgsSetter, `computeRequestPass`, `FrameRenderContext::createFromSetter`, `buildGraph` and `placeRenderGraph`. Do not copy `getRenderViewerArgsAndCheckCache_public` (:1729): it calls `createNewRenderRequest`, which bumps `renderAge` and aborts real renders, and M302 - Viewer Render State Cues reads `renderAge`.
  - verify: a dry run leaves `renderAge[2]`/`displayAge[2]` untouched and aborts no render in flight.
  - size: L
- [x] M116.P2.T3 — Queued signal to the GUI
  - files: `Engine/ViewerInstance.h`, `Engine/ViewerInstance.cpp`
  - approach: Emit a signal when the snapshot changes. Freeze it during playback.
  - verify: a signal-spy test sees one emission per change and none during playback.
  - size: M

## Phase 116.3: GUI

Waits for M118 - Large Graph GUI Responsiveness: it rewrites the same `Gui/NodeGraph*` and `NodeGui` code, with new hit-testing and low-zoom detail. Re-scout these tasks against it first.

- [x] M116.P3.T1 — Preference, shortcut and View-menu action on one state
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`, `Gui/NodeGraph35.cpp`, `Gui/NodeGraph25.cpp`, `Gui/GuiApplicationManager10.cpp`, `Gui/ActionShortcuts.h`, `Gui/ActionShortcuts.cpp`
  - approach: Copy the ShowLinkLines wiring above. The preference sits on the Node Graph page, default off. The action writes it through a `Settings::setXxx`.
  - verify: the View action, the shortcut and the preference stay in sync, checked by the UAT script.
  - size: M
- [x] M116.P3.T2 — Overlay item in the node graph
  - files: `Gui/GpuOverlay.{h,cpp}` (new), `Gui/NodeGraph.h`, `Gui/NodeGraphPrivate.h`, `Gui/NodeGui.cpp`, `Gui/Edge.h`, `Gui/CMakeLists.txt`
  - approach: Draw a region outline and a distinct outline for a runtime fallback. Draw a red X plus label on exit pipes only and a neutral tick with no text on entry pipes. Fade cached members. Identify a pipe by (dest `NodeGui`, `Edge::getInputNumber()`). Map group interiors to the enclosing group's `NodeGui` through `Node::getGroup()`.
  - verify: a unit test of the pipe mapping, then the UAT script.
  - size: L
- [x] M116.P3.T3 — Xvfb UAT script
  - files: `Tests/gui/m116_gpu_overlay.py`, `Tests/gui/guitest.py`
  - approach: Cover a mixed graph, a forced fallback, a cached member, toggling the overlay, and a no-GPU host, launched with `--gui`.
  - verify: the script passes and produces its screenshots, one file each.
  - size: M

**Verification gate:** one remote build per phase. Phases 1 and 2 run the new gtests plus `GpuPlacement_Test`, `FrameGraphBuild_Test`, `GpuRegionFallback_Test`, `ViewerRootOnDevice_Test` and `CacheStressGpu_Test` (the GPU ones on RADV). Phase 3 runs a `--gui` Xvfb UAT, one screenshot file per shot, shared with the user before sign-off. `tools/bench/gui/run.sh --xvfb` plus `compare.py` applies if M117 - GUI Perf Harness And Baseline has landed.

## Decisions

- 2026-11-07 — Phase 116.3 written: overlay drawn by `Gui/GpuOverlay.*` with no per-frame cost when off; Shift+G; `NodeGui.cpp`/`Edge.h` untouched. Remote GUI jobs have no GPU, so the mixed, cached and fallback cases need a local GPU run (joins the GPU host session).

- 2026-11-07 — User: start Phase 116.3 now, stacked on M118 - Large Graph GUI Responsiveness's branch (merged main, then milestone/m118 into milestone/m116 at 992731541). M116 merges after M118.

- 2026-11-05 — Elaborated per the 2026-10-25 decisions; the stub's Acceptance sketch is replaced by them. M116 - GPU Region Overlay's Phase 116.3 follows M118 - Large Graph GUI Responsiveness. Default shortcut Shift+G (PM): free in the nodegraph group.
- 2026-10-25 — Reasons are a category enum plus an optional effect detail string: this matches the stub's example reasons without giving every refusal branch its own message.
- 2026-10-25 — A runtime fallback is drawn as a distinct outline with the runtime reason: it shows both what was planned and what ran.
- 2026-10-25 — Scheduler-less viewer renders keep the last placement: a paint stroke or tracker step must not make the overlay flicker off.
- 2026-10-25 — The overlay draws a cache-ignoring shadow plan, with cached members faded: the user wants to see "what would have happened without the cache". `placeGraph` is a pure function of `PlacementInputs`, so a second pass with `isCached` false is graph-only work.
- 2026-10-25 — Multiple graphs per render are merged, and disagreement gets a "mixed" tag: split rects and A/B inputs can place the same node differently.
- 2026-10-25 — Entry pipes get a neutral upload tick and no text: uploads cost a transfer too, but the reason belongs to the upstream node.
- 2026-10-25 — Reasons appear only on exit pipes, with no node tooltip or badge: the user's choice, to keep the graph quiet.
- 2026-10-25 — Playback freezes the overlay until it stops: no repaint cost in the playback path.
- 2026-10-25 — Shadow placement and the GUI snapshot run only while the overlay is on: with the overlay off, the render path is unchanged apart from enum writes.
- 2026-10-25 — A dry-run planner fills the overlay on toggle-on and re-plans after every graph change, with no fade: the user wants the overlay current without needing a viewer render. This supersedes the earlier Out item "a dry-run placement without a render", and the stub's "a stale overlay is never shown" now means "replaced by a re-plan".
- 2026-10-25 — The preference lives on the Node Graph page, default off: it groups with the other node-graph display options.
- 2026-10-25 — The gate is engine gtests (reason categories, shadow placement, multi-graph merge, dry run) plus the affected GPU tests on RADV, plus a scripted Xvfb UAT with screenshots of outlines, markers, toggle, re-plan and the no-GPU case: per the user's choice and the RADV-before-merge rule.
- 2026-10-25 — No dependency on M117 - GUI Perf Harness And Baseline: the user chose not to wait for the bench.
- 2026-10-25 — The `Blocked on:` line has resolved: every E5 - GPU Compute dependency is `done`, and M87 - Point-Op Kernel Fusion was cancelled. The stub's Acceptance sketch predates these decisions ("cached on CPU" as a red X, the stale-overlay wording), so elaboration rewrites it per this section.
- 2026-11-10 — Main merged in at e0b72b82a; conflicts in Node.cpp, NodeGroup.cpp and Tests/CMakeLists.txt were resolved keeping both sides. Release gate with `m116_gpu_overlay.py` and `m55_node_graph.py` green (run 42766990775). The CI runner has no GPU, so its overlay shots show nothing placed. The last GPU-host shots (2026-11-10 00:58Z) predate fd0d2e294 and failed one fallback check. The fresh AppImage is staged for `BENCHES="m116shots m117"` on the GPU host; the user runs `push-bench.sh` and then the harness. The GPU tests on RADV remain for the GPU host.
- 2026-11-12 — User approved the screenshots and the UAT (user). Main is being merged in (bench harness and test conflicts), then the PR and review round.
- 2026-11-12 — Main merged in at 49dad69b1 (main's bench harness taken; both test files keep both sides). Release gate with m116_gpu_overlay and m55 running (run 43155474859). The branch carries M118 commits, so its PR waits for M118 (PR #259) to merge into main, which shrinks the diff to M116's own.
- 2026-11-12 — Main merged in at 49dad69b1; release gate green (run 43155474859). UAT and shots approved by the user. PR to main after M118 - Large Graph GUI Responsiveness merges and main is merged in again.
