# M364 - Delegate Cancel Responsiveness

Every renderer cancels quickly, and the UI never waits on a stop. In the viewer an old render can run on until the next sync. A renderer or settings change, or a large first or dirty-geometry sync, holds the shared stage lock across `engine->Render()`, so the GUI thread blocks on any of the 26 `stageMutex` sites in `Gui/SceneViewport.cpp`. The engine may only be destroyed under that lock, because its stage scene index stays subscribed to stage notices. Offline, cancel is checked only between renders, Storm runs up to 8 passes with no cancel check, and a request without an abort callback waits up to `kUnabortableRenderLimit` (600 s). Requested under F9 - Delegate Projection (user, 2026-11-07), with Natron Utility added the same day.

## Scope

**In:**
- A worker-owned stage for the progressive viewer worker, composed from the same node layers, used by every delegate (Storm, Cycles, MoonRay, and Natron Utility once it routes through the worker).
- The viewer worker never holds the GUI's stage lock across a sync or `Render()`, and destroys its engine without that lock.
- An edit, renderer change or settings change interrupts the pass in flight.
- Offline cancel inside a render for every delegate, Storm's GL passes included, and an abort callback on every render request.

**Out:**
- The Natron Utility delegate itself: F7 - Natron Utility Core plugs into the worker stage this milestone builds, and ships only after F9 - Delegate Projection lands.
- Display transform on delegate frames: M365 - Delegate Display Transform.

**Edge cases:**
- An edit lands while the worker is snapshotting layers: the snapshot is taken under the stage lock, so it sees all of an edit or none of it, and the generation bump schedules the next snapshot.
- A delegate without `StopRenderer` (Cycles, MoonRay): the pass is stopped by destroying its engine on the worker thread, which the GUI never joins.
- Closing the viewer mid-sync: `~ProgressiveRender` hands the worker to a reaper instead of joining it on the GUI thread.
- Live textures (M343 - Live Textures In Delegates) and the projections layer reach the worker stage like any other layer.

**Assumptions:**
- Copying the node layers into worker-owned anonymous layers (`SdfLayer::TransferContent`) under the lock is far cheaper than a sync. P1.T1 measures it on Lobby.

## Phase 364.1: Worker-owned stage

- [ ] M364.P1.T1 — Choose how the worker stage tracks the node layers
  - files: `Engine/Nodes/Scene/SceneStage.cpp`, `Engine/Nodes/Scene/SceneOps.cpp` (`openStage`), `Engine/Nodes/Scene/HydraRenderImpl.cpp` (`refine` ~:2989, `dropEngine` ~:2529), `## Decisions` here
  - approach: a stage opened on the same root layer shares `SdfLayer`s with the GUI, and reading a layer while the GUI writes it is a data race. Compare (a) worker-owned anonymous copies of the root, projections and payload sublayers refreshed per generation under the lock, against (b) the same with per-layer change tracking so only edited layers are copied. Measure the copy time and the time the lock is held on Lobby (`.local`) and on a generated 100k-prim scene. Record the numbers and the choice.
  - verify: numbers and one chosen design in `## Decisions`.
  - size: L

- [ ] M364.P1.T2 — Give the progressive worker its own stage
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/SceneStage.{h,cpp}`, `Tests/Scene/ProgressiveRender_Test.cpp`
  - approach: implement the P1.T1 design. `refine` takes the stage lock only to refresh its layer copies, then syncs and calls `engine->Render()` on its own stage without the lock. The engine subscribes to the worker stage only, so `dropEngine` no longer takes the stage lock. `pick()` reads the worker stage.
  - verify: `ProgressiveRender_Test` gains a case that holds the stage lock from the test thread for the length of a slow sync and asserts it can be taken again within 50 ms while the worker renders; the existing cases still pass.
  - size: L

## Phase 364.2: Fast stops

- [ ] M364.P2.T1 — Interrupt the viewer pass on any change
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (worker loop ~:3040, `setRenderer` ~:3153, `~ProgressiveRender` ~:3140), `Gui/SceneViewport.cpp` (`progressive.reset()` ~:1770)
  - approach: when `generation` moves during a pass, call `StopRenderer` where `IsStopRendererSupported()`, otherwise destroy the engine on the worker thread. `~ProgressiveRender` sets `quit` and hands the thread to a detached reaper that joins it, so the GUI thread never waits on a stop.
  - verify: `ProgressiveRender_Test`: with Cycles and MoonRay each mid-pass on a heavy generated scene, a settings change, a renderer change and destroying the render each return on the calling thread within 50 ms, and the next frame reflects the change.
  - size: M

- [ ] M364.P2.T2 — Cancel offline renders inside a pass
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (`render` ~:2049/:2159, render loop ~:1249-1290, Storm `_gl` branch ~:1230, `kUnabortableRenderLimit`), `Engine/Nodes/Scene/USDRender.cpp`, `Tests/Scene/RenderDelegates_Test.cpp`
  - approach: check cancel between Storm's GL passes and wire `request.aborted` for GL render threads too. Give every render request an abort callback (NatronRenderer's included) and drop the 600 s unabortable wait. Cycles and MoonRay stop by engine destruction as today; measure that join.
  - verify: `RenderDelegates_Test`: aborting a Storm, Cycles and MoonRay offline render mid-pass returns within 1 s each, with the measured times printed; `kUnabortableRenderLimit` and its wait path are gone.
  - size: M

**Verification gate:** `ProgressiveRender_Test`, `RenderDelegates_Test`, `CyclesDelegate_Test`, `MoonRayDelegate_Test` and `Tests/gui/m254_progressive_viewer.py` pass. A GUI script on Lobby switches renderer and changes a Cycles setting during the first sync, and the GUI thread never stalls more than 100 ms. GUI bench rule if the diff touches its paths. The user checks cancel feel on Lobby at F9 - Delegate Projection's UAT.

## Decisions

- 2026-11-07 — Worker-owned stage composed from the same node layers, used by every delegate (user): holding the shared stage lock across `engine->Render()` freezes the UI on a large first or dirty-geometry sync, for Natron Utility as much as Cycles and MoonRay.
- 2026-11-08 — Lives in F9 - Delegate Projection; F7 - Natron Utility Core ships only after F9 lands and rebases onto the worker stage (user).
