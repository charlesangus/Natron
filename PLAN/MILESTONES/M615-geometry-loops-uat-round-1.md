# M615 - Geometry Loops UAT Round 1

Fixes from the user's UAT of the F18 - Geometry Loops And Solver AppImage (2026-11-26), needed before F18 merges. Member of F18 - Geometry Loops And Solver. The image loop blocks (M180 - Loop Blocks, M181 - Loop Drivers) are the reference: geometry loops and solvers should look and behave like them wherever the concept carries over.

## Scope

**In:**
- Geometry For-Each and Solver blocks draw their hulls in the node graph, as image loops do.
- Each new geometry loop/solver gets a loop name filled in by default, as image loops do.
- The Solver exposes the full set of loop variables an image loop does (iteration, count/number of iterations, value, and so on), plus its own (substep, dt, frame), through knob drivers and wrangles.
- The Solver shows progress while it solves (a jump ahead, a re-sim): a progress indicator in the usual place for long renders, cancellable.
- "Drive from loop" works inside geometry loops and solvers, and their End nodes have the driven-from-loop table knob, as image loops do.
- Cache Simulation: the viewer timeline shows which frames are cached (the cache line, as for image renders), and the viewer indicates when a cached frame is being shown.
- GeoFile shows file sequences as sequences in the file dialog, defaults its filter to `*.ngeo`, and has the standard read frame handling knobs (frame range, before/after, offset, missing frames) like Read.
- GeoFileCache's file dialog defaults to `*.ngeo`.

**Out:**
- Anything not raised in this round.

## Phase 615.1: Loop blocks

- [ ] M615.P1.T1 — Hulls and default loop names for geometry loops and solvers
  - files: the image loop hull drawing and naming (M180), GeoForEachBegin/End, GeoSolverBegin/End
  - approach: register the geometry begin/end pairs with whatever the image loops use for hull drawing and default naming
  - verify: GUI script creates a For-Each and a Solver and checks the hull is drawn (screenshot) and each has a non-empty default loop name
  - size: M
- [ ] M615.P1.T2 — Loop variables, drive from loop and the driven table
  - files: M181 loop drivers, GeoForEachEnd, GeoSolverEnd, the loop variable registry
  - approach: the Solver publishes every image-loop variable that has a meaning for it, plus its own; "Drive from loop" in a knob's menu works for knobs inside geometry loops/solvers; their End nodes carry the driven-from-loop table knob
  - verify: ctest for the solver's variables; GUI script drives a GeoTransform from the solver and from a For-Each via the menu and checks the table rows
  - size: L
- [ ] M615.P1.T3 — Solver progress and cache visibility
  - files: GeoSolverEnd's solve walk, the progress reporting used by long renders, the viewer timeline cache line
  - approach: the solve walk reports progress (frames solved of frames needed) through the existing progress UI, cancellable; solved frames held in the sim cache appear on the timeline cache line, and the viewer shows when it is serving a cached sim frame
  - verify: GUI script jumps ahead on a slow solver, sees progress and cancels it; checks the cache line marks the solved frames
  - size: L

## Phase 615.2: GeoFile and GeoFileCache

- [ ] M615.P2.T1 — Sequences, filters and frame handling
  - files: GeoFile, GeoFileCache, the file dialog sequence support Read uses
  - approach: GeoFile's file knob uses the same sequence-aware file dialog as Read, with `*.ngeo` as the default filter; GeoFileCache's dialog defaults to `*.ngeo`; GeoFile gets Read's frame handling knobs (first/last frame, before/after, offset, missing frames) with the same behaviour
  - verify: ctest for frame handling; GUI script opens each dialog and checks the filter and a sequence entry
  - size: M

**Verification gate:** f18 gate with the M402-M404 GUI scripts plus the new ones; screenshots to the user.

## Decisions
- 2026-11-26 (PM) — Claimed; lane `.worktrees/m615` off the feature branch; all tasks dispatched to one opus implementer.
- 2026-11-26 (PM) — All tasks implemented in the next commit; m615-gate1 (--appimage, M402/M404 GUI scripts plus m615_loops/m615_geofile) queued.
  - **Shared loop interface:** one interface and registry for image, For-Each and Solver blocks. Loop names are unique across kinds (they share `loops.<name>.*`), and new or pasted blocks get the first free `loop<N>`.
  - **Solver variables:** iteration (substeps since the start frame), numiterations, count, t, value, frame, substep, numsubsteps, dt.
  - **Progress and cache:** progress shows when more than 2 frames need solving; cancel keeps the solved frames; the timeline marks kept frames; the viewer shows a "cached sim frame" label.
  - **GeoFile:** takes Read's frame knobs and logic wholesale. The old Missing Frames knob is gone (clean break). Black means empty geometry; the nearest-frame tie now goes to the next frame, as in Read; a file changed on disk is an error.
  - **`EffectInstance::onFilesReloaded()`:** a new hook so GeoFile re-lists its frames after a GeoFileCache write. It is a header change, so expect a full rebuild.
- 2026-11-26 (PM) — m615-gate2 found: a GeoSolverTest segfault (the preview context published a null previous frame as feedback), 'Drive from loop' driving only translate.x on an unfolded 3-dimension knob (added a 'Drive from loop (all dimensions)' entry), the cache's Filename dialog not openable through Python's openFile (plugin knob; the script clicks the button instead), and a 117 ms GUI stall in m402 (mostly CPU wait; the enclosing-loop walks per driven-knob read were trimmed). Fixed; m615-gate3 (--appimage) queued.
