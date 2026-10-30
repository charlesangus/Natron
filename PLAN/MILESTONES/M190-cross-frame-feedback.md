# M190 - Cross-Frame Feedback

A cross-frame mode for the loop block (user, 2026-10-25): at frame f > `startFrame`, LoopBegin outputs LoopEnd's result at f−1 instead of its input; at the start frame it outputs its input. It covers accumulating trails, paint build-up and temporal smoothing as well as stepped 2D simulations (reaction-diffusion, fluid-like effects, several substeps per frame), with both use cases weighted equally. Substeps are Feedback iterations within each frame. Each result is kept in a per-LoopEnd frame cache (RAM, under a bytes budget, with periodic checkpoint frames) whose entries go stale when the loop's hash changes. Missing frames are recomputed in order from the nearest cached frame. A disk bake works like Houdini's File Cache.

Acceptance sketch:
- A trail effect built with a loop in cross-frame mode accumulates correctly when played forward from the start frame, and re-renders correctly after an upstream edit.
- A reaction-diffusion graph with 4 substeps per frame plays back from a baked cache without recomputing earlier frames.
- Scrubbing backwards or jumping ahead renders correctly (recomputing from the nearest cached frame), never from a stale result.

## Scope

**In:**
- LoopEnd knobs `crossFrame` (Cross Frame), `startFrame` (Start Frame); works with Accumulate and Feedback.
- `LoopFrameCache`: RAM store keyed by (loop hash, context key, view, mipmap level, frame), with a budget and checkpoint eviction.
- A driver that renders the missing frames in order, can be aborted and shows progress.
- Bake to EXR: `cacheFile` (Cache File), `bakeEnd` (Bake End), `bake` (Bake), `loadFromCache` (Load From Cache), `clearCache` (Clear Cache).

**Out:**
- GPU placement of cross-frame tasks: M259 - GPU Placement Under Render Contexts.
- Deep/scene data: images only, as in M180.
- Python access to cached frames.

**Edge cases:**
- f < startFrame: LoopBegin passes its input through (no simulation yet).
- Scrubbing back, or jumping to f with no cache at f−1: recompute from the nearest cached frame g < f, else from startFrame. Never use an entry whose hash differs.
- Playback renders frames in parallel: frame f+1 waits on f's chain mutex. Work inside a frame stays parallel.
- Proxy/mipmap: each level is its own simulation (its own key).
- RFV contexts: keyed by context key, which folds the binding identity (decision 2026-10-26).
- An upstream edit changes LoopEnd's hash, so older-generation entries are purged and their RAM freed.
- loadFromCache on: baked frames load even after upstream edits, with a warning if the hash stored in the file differs. Frames past the bake continue from the last baked frame.

**Assumptions:**
- Each node hash covers that node's animation across all frames (curves are hashed), so a single hash per loop is enough for invalidation.

## Phase 190.1: Cross-frame cache and driver

- [x] M190.P1.T1 — Write the loop frame cache
  - files: `Engine/Nodes/Loop/LoopFrameCache.h`, `Engine/Nodes/Loop/LoopFrameCache.cpp` (new), `Tests/Native/LoopFrameCache_Test.cpp`
  - approach: map the key above to an ImagePtr. `nearestBefore(f)` returns the nearest cached frame. Budget: a share of the image-cache RAM setting. Over budget, evict the least-recently-used non-checkpoint frames first; frames at startFrame + k·`checkpointInterval` (chosen so the checkpoints fit within ¼ of the budget) go last. `purgeOtherGenerations(hash)`. Thread-safe.
  - verify: gtest checks insert/lookup, nearestBefore, which frames eviction keeps, the generation purge and a concurrent insert/lookup stress test.
  - size: M
- [x] M190.P1.T2 — Add cross-frame knobs, hashing and LoopBegin's back-reference
  - files: `Engine/Nodes/Loop/LoopEnd.{h,cpp}`, `Engine/Nodes/Loop/LoopBegin.{h,cpp}`
  - approach: add the knobs, shown only when Cross Frame is on (`refreshKnobVisibility`). Under crossFrame, `LoopBegin::appendNodeContentHash` folds the body digest (as under feedback, LoopBegin.cpp:457) plus `startFrame`, and the output is frame-varying. LoopBegin at iteration 0, f > start, outputs `LoopEnd::crossFrameImage(f−1)` (T3). The request pass adds no edge to f−1, so a frame's graph stays bounded. The stored window is LoopEnd's RoD, unclipped (user, 2026-10-28).
  - verify: at the phase build, `ctest -R NativeLoopCrossFrame` (T4) passes.
  - size: L
- [x] M190.P1.T3 — Drive missing frames from the nearest cached frame
  - files: `Engine/Nodes/Loop/LoopEnd.{h,cpp}`
  - approach: `crossFrameImage(f)`: on a cache hit, return it. Otherwise take the chain mutex (the FeedbackChain pattern, LoopEnd.cpp:333) and render frames g+1..f in order over the stored window, storing each frame. Show progress through `AppInstance::progressStart/Update/End` (Engine/AppInstance.h:332) when more than 2 frames need rendering, and stop if the render aborts. The legacy scheduler and the task-graph scheduler share it.
  - verify: at the phase build, `ctest -R NativeLoopCrossFrame` passes.
  - size: L
- [x] M190.P1.T4 — Test cross-frame feedback
  - files: `Tests/Native/NativeLoopCrossFrame_Test.cpp`
  - approach: trail: an animated dot with Merge max of the current dot and 0.9 × the previous frame; frame 10 matches the analytic sum; after a dot edit, frame 10 is recomputed. Jumping to frame 20 on an empty cache equals playing 1..20 in order; scrubbing back gives the same results. 4-substep Feedback (blur+grade step) equals 4·F single steps. A tight budget evicts frames and the recompute from a checkpoint is bit-identical. Results are bit-identical for 1 vs N threads and for both schedulers. f < start passes the input through.
  - verify: at the phase build, `ctest -R NativeLoopCrossFrame` passes.
  - size: M

## Phase 190.2: Bake

- [x] M190.P2.T1 — Bake frames to disk
  - files: `Engine/Nodes/Loop/LoopFrameBake.{h,cpp}` (new), `Engine/Nodes/Loop/LoopEnd.cpp`
  - approach: the Bake button drives startFrame..bakeEnd on a background thread through `crossFrameImage`. Each frame is written as float EXR with `OiioWriteSupport`, with a `natron:loopHash` attribute. Clear Cache empties RAM only.
  - verify: gtest: baking 1..6 writes 6 files carrying the current hash.
  - size: L
- [x] M190.P2.T2 — Load baked frames
  - files: `Engine/Nodes/Loop/LoopEnd.cpp`, `Engine/Nodes/Loop/LoopFrameBake.cpp`
  - approach: with loadFromCache on, a baked frame is read (`OiioReadSupport::readHeader` + read) before the RAM cache or the driver. A hash mismatch sets a persistent warning. Missing frames continue from the last baked frame.
  - verify: gtest: bake a 4-substep simulation over 1..12, clear the RAM cache; frame 12 renders with 0 body renders (counter) and bit-identical to the computed result. An upstream edit warns.
  - size: M
- [x] M190.P2.T3 — GUI check and docs
  - files: `Tests/gui/m190_cross_frame.py`, `Engine/Nodes/Loop/docs/LoopEnd.md`
  - approach: under Xvfb: the knobs carry the labels above. Bake shows progress, then the trail plays back from the bake. Screenshots. Docs cover semantics, the budget/checkpoints, the bake and the stored window (LoopEnd's RoD).
  - verify: the script passes under the remote GUI runner; docs reviewed at the gate.
  - size: M

**Prior verification gate:** full build + ctest incl. `LoopFrameCache`, `NativeLoopCrossFrame`; `tools/ci/remote/build.sh debug --tests` green; Xvfb trail and reaction-diffusion projects rendered, screenshots shared with the user; AppImage for UAT.

## Decisions

- 2026-10-28 — **Respect the RoD; don't clip to the input format (user).** Trails that leave the frame are kept.
- 2026-10-28: elaborated (consultant). Defaults from its recommendations, open to the user's override: the RAM budget is a share of the image-cache setting, not a knob; Load From Cache keeps loading after upstream edits with a warning, as Houdini does.
- 2026-10-26: RFVs bind to Set knobs and evaluate at the requested frame (M71 Phase 71.5); cross-frame results are keyed by binding identity and frame, so rendering N-1 from N carries RFV(N-1).
- 2026-10-25 — **Design for trails and simulations equally (user).**

## Phase 190.3: Fractional-frame iteration

- [ ] M190.P3.T1 — Evaluate loop samples at fractional frame times
  - files: `Engine/Nodes/Loop/*`, render-context time keys, `Tests/Native/NativeLoopCrossFrame_Test.cpp`, loop docs
  - approach: Give iteration/substep sampling an explicit fractional time derived from the loop range and count, propagate it through input evaluation and caching, and document the distinction between iterations and whole-frame stepping.
  - verify: An animated-dot fixture sampled over several substeps produces smooth intermediate positions and distinct cache keys rather than repeated whole frames.
  - size: L

**Verification gate:** the original cross-frame gate passes; the animated-dot UAT demonstrates smooth fractional samples and the guide describes the semantics accurately.
