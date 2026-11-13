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

- [x] M364.P1.T1 — Choose how the worker stage tracks the node layers
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
- 2026-11-12 — **P1.T1: the worker shares the node layers and keeps its own `SceneStage` (spike `tools/spikes/worker-stage/measure.py`).** The GUI writes only the root and projections layers, which `SceneStage` owns. Node layers in a payload are never edited (`SetPermissionToEdit(false)`), and file layers change only under `SceneWriteBarrier::runExclusive`. So two stages reading the shared layers is safe, and anonymous copies would break file layers' relative anchors. Copying under the lock costs 2.2–2.6 s at 100k prims, while a shared-layer snapshot holds the lock 0.1–0.2 ms (Lobby 0.06–0.12 ms). Small-edit syncs on the worker stage cost 1.5 ms on Lobby and 42 ms at 100k, and stay incremental (one changed property). A heavy recook rebuilds that layer's prims, as the GUI stage already does today.
  - **Design:** the worker owns a persistent second `SceneStage`. It snapshots the `ScenePayload` (shared-pointer copy), the preview deltas with their time code, and the generation under `stageMutex` (lock order `stageMutex` then the worker `mutex`), then runs `sync()`/`syncProjections()` and `Render()` outside the lock. It stays a `SceneWriteBarrier` participant with its own pause/resume. `dropEngine` needs only `engineMutex`, and the engine goes before the worker stage. `pick()` drops `stageLock` and `try_lock`s `engineMutex`. The viewport's quiesce stops waiting on a running render.
  - **Not adopted now:** in-place mirroring per stack slot (2.9 s at 100k with one changed property, against 5.3 s plus a full rebuild on a heavy recook). It's a possible later optimisation, at double the in-memory layer memory and with slot keying to solve.
- 2026-11-12 — P1.T2 implemented (41f88b159). The worker owns a persistent `SceneStage` synced from a `SceneStageSnapshot` (payload, preview deltas, time code). `pick()` `try_lock`s `engineMutex`. The worker is its own `SceneWriteBarrier` participant.
- 2026-11-12 — **P2.T1 implemented (2f7757c71), PM engineering call on the stop mechanism.** Neither Cycles nor MoonRay has `StopRenderer` (Cycles has Pause/Resume, and `hd_moonray` exports only Pause/Resume). For plain edits and camera moves the engine is not destroyed, because that would turn every orbit into a full resync. The delegate's own reset at the next worker sync interrupts its render, so the GUI never waits. A renderer, settings or thread-count change drops the engine on the worker thread as soon as the delegate call in flight returns. `~ProgressiveRender` detaches a self-ending worker, and `~AppManager` drains workers that are still ending, with a 30 s warning limit. The barrier quiesce still waits for the one delegate call in flight, because that call reads the file layers a reload edits. Pause-based interruption and handing a busy engine to a reaper are possible follow-ups. The user judges cancel feel at F9's UAT.
- 2026-11-13 — Gate fixes: the Storm abort test now aborts a warm engine's pass, since the cold first pass on llvmpipe is mostly shader compilation and Storm keeps its engine (d2d0343a7). The responsiveness GUI script was missing from the tasks and is added (`Tests/gui/m364_cancel_responsiveness.py`, 020ab0f4e); its generated scene had no meshes and is fixed (84600c9c8). The coalescing test counts engine rebuilds under load. **Switching back to Storm re-syncs the whole scene on the GUI thread, a stall of about 5.5 s (PM call: fix in this milestone).** The viewer's Storm engine and its synced scene stay alive while another renderer is active, released on Free Memory or memory pressure, so switching back is incremental. In progress.
