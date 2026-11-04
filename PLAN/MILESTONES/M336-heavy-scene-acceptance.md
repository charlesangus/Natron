# M336 - Heavy Scene Acceptance

The end-to-end proof that M231 - Scene Graph Panel lacked: every budget together on the
fixtures, then the user on a real production scene. Design:
`PLAN/DESIGN/2026-11-04-scene-graph-design.md`.

## Scope

**In:**
- One scripted end-to-end run of every budget on both fixtures.
- A UAT with a real scene such as ALab or Moana Island, kept outside the repo.
- Fixes for what those find, as new tasks here.

**Out:**
- Storing a production scene in the repo or LFS: never.
- New features the UAT suggests: new milestones.

**Edge cases:**
- A budget revised after M327 - Heavy Scene Spike applies as revised. No second revision without the user.
- A real scene with no authored bounds opens as markers, by design (user). That's a finding to discuss, not a bug.

**Assumptions:**
- The user has, or can download, a production scene. Which one is the user's choice at UAT.

## Phase 336.1: Budgets

- [ ] M336.P1.T1 — End-to-end budget run
  - files: `tools/bench/heavy_scene_bench.py`, `tools/bench/README.md`
  - approach: add a scenario chaining the whole workflow on both fixtures: open, twirl into a payload, set Full on one asset, Full + Load on a block, add a pattern node, switch to another node, select in the inspector, save, reopen. Run on a stripped release build on a GPU host and record the JSON under `## Decisions`.
  - verify: every budget passes; any failure becomes a task here with its numbers.
  - size: M

## Phase 336.2: Real scene

- [ ] M336.P2.T1 — UAT AppImage and steps for a real scene
  - files: `build/appimages/` (AppImage plus same-named steps `.md`)
  - approach: package a stripped release AppImage after all of M329 - Viewer Draw States, M330 - Graph Draw Policy, M331 - Deferred Pattern Matching and M332 - Scene Graph Points are on main. The steps walk all five goals on the user's scene, and ask for times on open, reveal and orbit.
  - verify: the user's report is recorded here, with any fixes filed as tasks.
  - size: M

**Verification gate:** end-to-end run passes every budget on the GPU host; the user accepts the behaviour on a real production scene.
