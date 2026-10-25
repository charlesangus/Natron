# M116 - GPU Region Overlay

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Make the scheduler's GPU placement visible in the node graph, so users can see how the comp is being optimized and restructure it to keep work resident on the GPU. A subtle outline wraps the nodes and pipes of each resident region that M84 - GPU Placement And Residency's placement pass formed (`FrameGraph::Task::regionId` and `FrameGraph::Region`, `Engine/RenderScheduler.h`; `placeGraph` in `Engine/GpuPlacement.cpp`). Every pipe where a region ends gets a red X, with short text beside it that says why the region stopped there. The overlay shows the regions of the frame the active viewer rendered most recently, and updates after each viewer render (user, 2026-10-13).

Today `placeGraph` drops or splits candidates without recording why, and nothing passes placement to the GUI. This milestone adds both:
- an end reason for each region exit, recorded where the placement pass decides it;
- the last viewer render's placement, published to the GUI.

It reuses the precedent of the node graph's Show Expressions toggle: a View-menu action with a node-graph shortcut (`kShortcutIDActionGraphShowExpressions`, `Gui/NodeGraph35.cpp:445`, `Gui/GuiApplicationManager10.cpp:925`). The difference is that this toggle also persists as a preference.

Blocked on: the rest of E5 - GPU Compute. The user placed this milestone at the end of the epic (2026-10-13). Region formation is still changing: M85 - Native GPU Kernels ports most nodes, M109 - GPU Region Engine Hardening makes multi-plane tasks candidates, and M87 - Point-Op Kernel Fusion may fuse members. The set of end reasons and where they are decided is only stable once those land.

Acceptance sketch:
- With the overlay on and a GPU present, a comp's GPU regions after a viewer render are drawn as subtle outlines around their member nodes and the pipes between them. Neighbouring regions can be told apart, and the outline stays readable in a dense graph without hiding node colours or labels.
- Every pipe leaving a region for a consumer outside it carries a red X with a one-line reason. Example reasons: "no GPU path", "mask input unsupported on GPU", "multi-plane", "cost model: transfer outweighs gain", "cached on CPU", "would form a cycle", "out of device memory → CPU fallback".
- Changing the graph or the viewed frame and rendering again updates the overlay to the new placement. A stale overlay is never shown for a graph that changed since its render.
- A preference turns the overlay on or off and persists across sessions. A node-graph shortcut and a View-menu action toggle the same state.
- With GPU compute off, or with no GPU, enabling the overlay draws nothing and does not error.

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

## Decisions

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
