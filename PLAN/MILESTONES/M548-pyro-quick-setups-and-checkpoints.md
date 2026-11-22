# M548 - Pyro Quick Setups And Checkpoints

The last `PyroSolver` controls: the Quick Setups menu that builds common networks (sources, smoke, fire, explosion, collision, look, lights, camera, render), simulation checkpoints, and a params→knob audit of all five pyro nodes against the reference control lists, so F35 - Pyro ships every control.

Member of F35 - Pyro: branches off and merges into `feature/f35-pyro` (PLAN-FORMAT §9b); the Feature's UAT covers it. Epic: E22 - Simulation And Solvers. Shared design: `PLAN/DECISIONS/2026-11-22-vdb-pyro-solver.md`.

## Scope

**In:**
- Quick Setups: Initialize Sources, Initialize Smoke, Initialize Fire, Initialize Explosion, Add Color Source, Reference Bound, Setup SDF Collision, Create Pyro Look, Create Lights, Create Lights/Cameras, Create Render Stage, Sharpen Volume, Cache Simulation. Each builds and connects native nodes around the selected `PyroSolver` (`PyroSource`, `VolumeRasterizeAttributes`, `VDBFromPolygons`, `PyroBakeVolume`, USD lights and camera, `USDRender`, `GeoFileCache`) as one undoable action.
- Checkpoints: Save Checkpoints, Base Name, Base Folder, Version, Checkpoint Trail Length, Checkpoint Interval; resume from the latest checkpoint after the in-memory cache is gone.
- Audit: every control on the reference lists for `PyroSolver`, `PyroSource`, `VolumeRasterizeAttributes`, `PyroBakeVolume` and `PyroPostProcess` is present with its default, or recorded as dropped with a reason.

**Out:**
- New solver features found missing by the audit beyond knob wiring: each becomes a new F35 - Pyro member through `/cat-plan`.

**Edge cases:**
- A Quick Setup run with no `PyroSolver` selected: creates one.
- Checkpoint files from a different solver hash: ignored, with an info message.
- Checkpoint trail length 0: keep every checkpoint.

## Phase 548.1: Setups and checkpoints

- [ ] M548.P1.T1 — Quick Setups menu actions
  - files: `Engine/Nodes/Pyro/PyroQuickSetups.{h,cpp}`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroQuickSetups_Test.cpp`
  - approach: Each menu entry builds its nodes through the node-creation API used by Make I-Node (one undo macro), positions them beside the solver and sets the knobs the setup needs (for example Initialize Explosion: flame source, expansion, temperature emission). Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: `PyroQuickSetups_Test`: each entry creates the expected node types and connections; one undo removes them all.
  - size: M
- [ ] M548.P1.T2 — Checkpoints
  - files: `Engine/Geo/Pyro/PyroCheckpoint.{h,cpp}`, `Engine/Nodes/Pyro/PyroSolver.{h,cpp}`, `Tests/Native/PyroCheckpoint_Test.cpp`
  - approach: Every Checkpoint Interval frames write the full solver state (all fields, staggered velocity, reflection field, active topology) with `GeoFileFormat` (M403 - Geometry File Cache) to `<Base Folder>/<Base Name>.v<Version>.<frame>`, pruning past the trail length; on a cold cache, resume from the newest checkpoint whose solver hash matches.
  - verify: `PyroCheckpoint_Test`: sim to 20 with interval 5, clear the cache, request 22: steps run only for 21–22, and frame 22 matches an uninterrupted run within tolerance.
  - size: M

## Phase 548.2: Audit and proof

- [ ] M548.P2.T1 — Params→knob audit
  - files: `Engine/Nodes/Pyro/*.cpp` (read), `.local/docs/houdini/nodes/sop/{pyrosolver,pyrosource,volumerasterizeattributes,pyrobakevolume,pyropostprocess}.md` (read)
  - approach: List every reference parameter per node and match it to a knob and default in the code; report present / missing / dropped-with-reason as a table for the PR body only (never committed, never naming the reference application in the repo).
  - verify: the table covers every reference parameter of the five nodes; any missing knob is listed for a follow-up fix task.
  - size: M
- [ ] M548.P2.T2 — GUI check: explosion end to end
  - files: `Tests/gui/m548_pyro_explosion.py`, `Engine/Nodes/Pyro/docs/PyroSolver.md`
  - approach: Run Initialize Explosion and Create Render Stage on an empty graph, cache 48 frames headless with `GeoFileCache`, and render frame 24 in Storm; docs for Quick Setups and checkpoints written fresh.
  - verify: the script passes under Xvfb; the headless cache matches the interactive result hash for hash; renders sent with SendUserFile for the Feature's UAT.
  - size: M

**Verification gate:** full ctest green including `PyroQuickSetups_Test` and `PyroCheckpoint_Test`; `Tests/gui/m548_pyro_explosion.py` passes under Xvfb; the audit table shows no missing control.
