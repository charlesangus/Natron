# M302 - Viewer Render State Cues

Make the UI say truthfully what the renderer is doing. The viewer gets a coloured border while the image it shows is stale. Node "rendering" dots light only when a node actually executes, on either device, and their colour says which device. Today the only "render in flight" cue is the refresh button's icon swap (`Gui/ViewerTab40.cpp` ~:767-776), and nothing says "up to date". The node dot (`Gui/NodeGui.cpp` ~:2728) lights only for CPU execution after a cache miss (`Engine/EffectInstanceRenderRoI.cpp` ~:1885), never for GPU region members. Input edges light even on cache hits (`Engine/ParallelRenderArgs.cpp` ~:448). The dot is polled every 300 ms and throttled to one update per second (`Engine/Node.cpp` ~:130, :5951).

## Scope

**In:**
- Per-viewer stale/current state in the engine.
- An amber border on the viewer while stale for longer than a user-set delay (preference, default 150 ms); it clears the moment the image is current.
- Node dots lit on real execution only, on either device, coloured by device (CPU vs GPU). Executions latched between polls so short renders still show.
- Edges lit only when pixels flow from an executing upstream node.

**Out:**
- A failed/aborted state or a draft state on the border (user: stale and current only).
- Progress bars or render-time readouts.
- The timeline cached-frame line (unchanged).

**Edge cases:**
- **A/B wipe:** the viewer is stale if either displayed input is stale.
- **An aborted render with a newer one queued:** stays stale until the newer one lands.
- **Playback:** frames served from the viewer cache are current; the border does not flash per frame.
- **A render that fails with nothing newer queued:** the border clears (the image is as current as it will get), and the existing error display reports the failure.
- **A paused viewer** (render on demand / auto-refresh off): stale shows while a manual refresh is pending.

**Assumptions:**
- The viewer can compare the graph age it is displaying with the age of the latest requested render without new locking on the render path.

## Decisions

- 2026-11-06 — Freshness check: P1.T1 rebriefed onto the existing ages. M118 - Large Graph GUI Responsiveness replaced the idle poll with a dirty set (`NodeGraph::scheduleNodeRenderStateRefresh`, `NodeGraph35.cpp:174`, feeding `refreshRenderStateTimer`; `NodeGui::refreshRenderingIndicator`, `NodeGui.cpp:2850`), so Phase 302.2's dot and edge tasks wait for it and hook that path, not the 300 ms loop their briefs describe. M116 - GPU Region Overlay's dry run must leave `renderAge` alone.
- 2026-11-01 — **Coloured viewer border, stale and current states only (user).**
- 2026-11-01 — **The border delay is a user preference (user);** default 150 ms (planner).
- 2026-11-01 — **Dots mean real execution, coloured by device (user).**

## Phase 302.1: Viewer stale state

- [x] M302.P1.T1 — Track stale vs current per viewer
  - files: `Engine/ViewerInstance.h`, `Engine/ViewerInstance.cpp`, `Engine/ViewerInstancePrivate.h`, `Tests/ViewerStaleState_Test.cpp` (new)
  - approach: Don't add a request age: derive stale from `renderAge[2]` against `displayAge[2]` (`ViewerInstancePrivate.h:157-164`), using `isLatestRender` and `checkAndUpdateDisplayAge`, per input for A/B and after abort coalescing. Emit `staleChanged(bool)` on transitions. Viewer-cache playback frames count as current.
  - verify: A test requests renders, aborts one, and checks the stale/current transitions and the A/B rule.
  - size: M

- [ ] M302.P1.T2 — Draw the stale border
  - files: `Gui/ViewerGL.cpp`, `Gui/ViewerTab40.cpp`, `Engine/Settings.cpp`, `Engine/Settings.h`
  - approach: On `staleChanged(true)` start a timer for the preference `viewerStaleBorderDelayMs` (default 150, under Viewer preferences). When it fires, draw a 2-px amber border inside the GL viewport. `staleChanged(false)` cancels the timer and clears the border at once.
  - verify: A GUI script under Xvfb holds a slow render and finds the border pixels amber after the delay and clear after completion; screenshots go to the user.
  - size: M

## Phase 302.2: Truthful node highlights

- [ ] M302.P2.T1 — Light GPU region members that execute
  - files: `Engine/GpuRegionTask.cpp`, `Engine/Node.cpp`, `Engine/Node.h`, `Engine/RenderScheduler.cpp`
  - approach: Count executions per node per device. A GPU region increments the GPU count for each member that actually runs, using the member intervals it already records; members served from a store or a cache don't count. Executions since the last poll are latched, so a 5 ms render still shows once.
  - verify: A unit test runs a two-member GPU region and reads one GPU execution per member; a cache-served member reads zero.
  - size: M

- [ ] M302.P2.T2 — Stop lighting edges on cache hits
  - files: `Engine/ParallelRenderArgs.cpp` (~:448-450), `Engine/EffectInstanceRenderRoI.cpp`, `Gui/Edge.cpp`
  - approach: An input edge lights only when its upstream node executed for this render, not when a consumer merely pulled the input from a cache.
  - verify: A unit test with a cached upstream shows no edge lit; with a missing upstream, it is lit.
  - size: M

- [ ] M302.P2.T3 — Colour the dot by device
  - files: `Gui/NodeGui.cpp` (~:2728-2775), `Gui/NodeGraph35.cpp` (poll ~:271)
  - approach: The dot reads both device counters: yellow for CPU as today, a distinct colour for GPU, split half and half if both ran. The poll interval is unchanged.
  - verify: A GUI script under Xvfb renders a mixed CPU/GPU graph; screenshots show the expected colours and go to the user.
  - size: M

**Verification gate:** unit suites pass. GUI scripts under Xvfb show the border after the delay and clearing on completion, and device-coloured dots only on executing nodes. The user approves the screenshots.
