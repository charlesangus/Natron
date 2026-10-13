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
- Recording a reason for each region exit in the placement pass, and in the region body for runtime fallbacks.
- Publishing the last viewer render's placement to the GUI.
- The outline, the red-X markers and their labels in the node graph.
- The preference, the shortcut and the View-menu action.

**Out:**
- Changing placement decisions or the cost model. This milestone only reports them.
- Per-node GPU timings or transfer sizes in the graph. Profiles already carry them (M84 - GPU Placement And Residency).
- A dry-run placement without a render. The user chose the last viewer render as the source.
- Kernel fusion (M87 - Point-Op Kernel Fusion).

**Edge cases:**
- A region feeding the viewer: the viewer is the natural end, so it gets no X. Until M107 - GPU Viewer Hand-off lands, it is still a download.
- A region crossing a Group node's boundary: in the parent graph, outline the Group node itself. Inside the group, outline its members.
- Several viewers: track the active viewer.
- Placement that differs between views or frames: show only what the last viewer render placed.

**Assumptions:**
- The overlay is off by default.
- The default shortcut is chosen at elaboration, after checking for conflicts in the node-graph shortcut group.
