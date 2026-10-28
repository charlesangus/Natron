# M125 - Native Multi-Frame Nodes

This milestone ports the openfx-misc nodes that read several frames as redesigned native nodes. Each uses a plain ID at major 1, is scene-linear, and gets mask, mix and premult from the base. The nodes are:

- `FrameBlend`, `TimeBlur` and `NoTimeBlur`, kept as three separate nodes;
- `AppendClip`, which concatenates its inputs in time with fades and cross-dissolves;
- `SlitScan`, which gathers each pixel from a frame chosen by a retime map, within a bounded frame window.

Every node runs on the GPU, using the multi-time GPU views from M124 - Native Time Remaps.

TimeDissolve is retired rather than ported: animating the native Dissolve's `which` covers it. TimeBufferRead and TimeBufferWrite are retired too.

Deps: M124 - Native Time Remaps, for the multi-time fetch, the (input, time) GPU views, and the generic N-view blend kernel `timeblend.slang`.

Scouted facts (scouted 2026-10-13 on main `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion):
- **OFX sources** (`build/assets/plugin-src/openfx-misc/`):
  - **FrameBlend** (`FrameBlend/FrameBlend.cpp`):
    - Knobs: `frameRange` (default -5,0), `absolute`, `frameInterval` (1), `operation` (Average, Min, Max, Sum, Product, Over), `decay`, `outputCount`, an optional `FgM` clip, process RGBA, mix and mask.
    - `getFramesNeeded` (`:1028`) returns one range, or one single-frame range per sample when the interval is above 1.
    - Render fetches the frames in chunks. A single-frame range is identity.
  - **TimeBlur** (`TimeBlur/TimeBlur.cpp`):
    - Knobs: `division` (10), plus shutter (0.5), shutter offset and custom offset from `SupportExt/ofxsShutter.cpp`.
    - It averages `division` fractional times (`getFramesNeeded`, `:457`) in chunks of 4.
  - **NoTimeBlur** (`TimeBlur/NoTimeBlur.cpp`): `rounding` (rint, floor, ceil, none). Identity at the rounded time.
  - **AppendClip** (`AppendClip/AppendClip.cpp`):
    - Knobs: `fadeIn`, `fadeOut`, `crossDissolve`, `firstFrame` (1), a read-only `lastFrame`, and an `updateLastFrame` button.
    - It takes up to 64 inputs. `getSources` maps the time to one or two active clips.
    - `getFramesNeeded` is at `:772`, `isIdentity` at `:713`, and `getTimeDomain` is `[first, last]`.
  - **SlitScan** (`SlitScan/SlitScan.cpp`):
    - Knobs: `retimeFunction` (horizontal slit, vertical slit, retime map), `retimeOffset`, `retimeGain` (-10), `retimeAbsolute`, `frameRange` (-10,0), and `filter` (Nearest, Linear).
    - The per-pixel source time is `offset + gain*ramp`. `getFramesNeededRange` is at `:324`.
  - **TimeDissolve** (`TimeDissolve/TimeDissolve.cpp`): two inputs mixed by a weight driven by time, not a time remap.
  - **TimeBufferRead/Write** (`TimeBuffer/TimeBuffer.cpp`):
    - Stateful feedback through a global buffer map.
    - Sets `SequentialRender` (`:779`), with a `#pragma message` at `:57` saying Natron doesn't support it.
- **References:**
  - `Engine/EffectInstance.h:71` (`PLUGINID_OFX_TIMEDISSOLVE`).
  - `Engine/OfxEffectInstance.cpp:1298-1309, 1338` (default-channel lists; TimeDissolve is under Routing) and `Engine/OfxParamInstance.cpp:4850-4854` (TimeDissolve curve default).
  - Tests: `Tests/FrameGraphBuild_Test.cpp:74, 514-548`, `SchedulerEquivalence_Test.cpp:411, 860`, `GpuResidency_Test.cpp:828-836`, `TimeVaryingLayers_Test.cpp:474-482`, `DefaultChannelSet_Test.cpp:80-91`, `ColorViewsRender_Test.cpp:92-93` (TimeBuffer) and `NodeCategory_Test.cpp:168`.
  - Bench: `tools/bench/comps/build_comps.py:249-254` and `defocus_retime.ntp` use TimeBlur, Retime and FrameBlend.
- **Cost:** FrameBlend is one of the top costs in the render profile (`.plan/PLAN/DESIGN/2026-10-10-render-cost-profile.md:64,70`: 21 ms/Mpx, about 2253 ms total).
- **Native Dissolve:** `Engine/Nodes/Merge/Dissolve.*`, with 64 inputs, an animatable `which`, and `getFramesNeeded` at `:441`.

## Scope

**In:**
- Native `FrameBlend`, `TimeBlur`, `NoTimeBlur`, `AppendClip` and `SlitScan`, each with a CPU kernel and a GPU kernel.
- A frame-array gather kernel for SlitScan.
- TimeDissolve, TimeBufferRead and TimeBufferWrite retired.
- A before/after measurement of FrameBlend on `defocus_retime`.

**Out:**
- Time remaps and the multi-time infrastructure: M124 - Native Time Remaps.
- A dedicated TimeDissolve replacement: none. Animate the native Dissolve's `which` (user).
- Optical-flow frame blending: none.
- Renaming native Dissolve: M111 - Native Node Identity Cleanup.

**Edge cases:**
- **FrameBlend with a single-frame range:** identity at that frame.
- **FrameBlend with an interval above 1:** fetches only the sampled frames, not the whole range.
- **TimeBlur with shutter 0 or one division:** identity. Fractional sample times are passed upstream, and a `NoTimeBlur` upstream rounds them.
- **AppendClip with disconnected inputs:** they are skipped when computing the timeline. A cross-dissolve spanning a gap fades to black.
- **SlitScan when the map asks for frames outside the declared window:** the time clamps to the window. It never makes an undeclared fetch.
- **More frames than the GPU view cap:** FrameBlend accumulates in chunks across dispatches, keeping the region resident.

**Assumptions:**
- Unconfirmed: TimeBufferRead and TimeBufferWrite are retired without a replacement. Their stateful feedback needs sequential rendering, which the task-graph scheduler doesn't support.
- Unconfirmed: FrameBlend keeps the Average, Min, Max, Sum and Over operations and drops Product. Decay stays, as an exponential weight.
- Unconfirmed: SlitScan's frame window is capped (default 64 frames) to bound memory, and the cap is shown in the node's message when it clips.
- Unconfirmed: knob names follow Nuke where Nuke has the node: FrameBlend `startframe`/`endframe` and TimeBlur `divisions`, `shutter` and `shutteroffset`.

## Phase 124.1: Blend nodes

- [ ] M125.P1.T1 — Native FrameBlend on the CPU
  - files: `Engine/Nodes/Time/FrameBlend.{h,cpp}` (new), `Engine/Nodes/Time/FrameBlend.reg.inc`, `Tests/Native/NativeFrameBlend_Test.cpp` (new)
  - approach:
    - Relative or absolute range, interval, operation, decay, and output count to alpha.
    - `getFramesNeeded` lists each sample, and the samples are fetched through M124 - Native Time Remaps's `fetchInputAt` and accumulated in chunks.
    - The optional foreground-matte input is dropped; use the host mask instead.
  - verify: tests cover each operation, the interval, decay, absolute mode and the single-frame identity, under both scheduler modes. Runs at the phase build.
  - size: L

- [ ] M125.P1.T2 — Native TimeBlur and NoTimeBlur on the CPU
  - files: `Engine/Nodes/Time/TimeBlur.{h,cpp}` (new), `Engine/Nodes/Time/NoTimeBlur.{h,cpp}` (new), `Engine/Nodes/Time/{NoTimeBlur,TimeBlur}.reg.inc`, `Tests/Native/NativeTimeBlur_Test.cpp` (new)
  - approach:
    - TimeBlur takes divisions, shutter and shutter offset (centred, start, end, custom), and averages fractional sample times.
    - NoTimeBlur rounds the time (round, floor, ceil) through M124 - Native Time Remaps's time-remap hook, so it is identity at the rounded time.
  - verify: tests cover the sample times for each offset mode, the identity at shutter 0, and NoTimeBlur upstream of TimeBlur collapsing the samples to integral frames. Runs at the phase build.
  - size: M

- [ ] M125.P1.T3 — FrameBlend and TimeBlur on the GPU
  - files: `Engine/Gpu/Kernels/timeblend.slang`, `Engine/Gpu/Kernels/timeblend.cmake`, `Engine/Nodes/Time/FrameBlend.cpp`, `Engine/Nodes/Time/TimeBlur.cpp`, `Tests/Native/NativeFrameBlendGpu_Test.cpp` (new)
  - approach:
    - Extend M124 - Native Time Remaps's N-view blend with the operation modes (min, max, sum, over) and per-view weights for decay.
    - Above the view cap, chain dispatches through an accumulator buffer.
    - Both nodes fill the views through `makeGpuKernel`.
  - verify: the GPU parity harness matches the CPU within 1e-5 for every operation and for a sample count above the cap. Runs at the phase build on RADV.
  - size: L

## Phase 124.2: AppendClip and SlitScan

- [ ] M125.P2.T1 — Native AppendClip
  - files: `Engine/Nodes/Time/AppendClip.{h,cpp}` (new), `Engine/Nodes/Time/AppendClip.reg.inc`, `Tests/Native/NativeAppendClip_Test.cpp` (new)
  - approach:
    - 64 optional inputs (the Dissolve pattern), `first_frame`, fade in, fade out and cross-dissolve.
    - `getFrameRange` sums the connected inputs' ranges.
    - It is identity on one active clip, and blends two clips through the multi-time fetch during a dissolve.
  - verify: tests cover three clips with a cross-dissolve, fades, a disconnected middle input, and the reported range. Runs at the phase build.
  - size: M

- [ ] M125.P2.T2 — Native SlitScan
  - files: `Engine/Nodes/Time/SlitScan.{h,cpp}` (new), `Engine/Nodes/Time/SlitScan.reg.inc`, `Tests/Native/NativeSlitScan_Test.cpp` (new)
  - approach:
    - Horizontal slit, vertical slit, or retime map from a second input's channel (via the standard channel knob), with offset, gain, absolute mode and the Nearest or Linear filter.
    - Declare the window's frames in `getFramesNeeded` and gather each pixel from them.
  - verify: tests cover each mode on a synthetic frame-numbered sequence, the Linear filter between frames, and clamping to the window. Runs at the phase build.
  - size: L

- [ ] M125.P2.T3 — AppendClip and SlitScan on the GPU
  - files: `Engine/Gpu/Kernels/slitscan.slang` (new), `Engine/Gpu/Kernels/slitscan.cmake`, `Engine/Nodes/Time/SlitScan.cpp`, `Engine/Nodes/Time/AppendClip.cpp`, `Tests/Native/NativeSlitScanGpu_Test.cpp` (new)
  - approach:
    - SlitScan uses a frame-array gather over the window's views, reading the per-pixel time from the map view.
    - AppendClip's dissolve reuses the two-view blend.
  - verify: the GPU parity harness matches the CPU within 1e-5 for both nodes. Runs at the phase build on RADV.
  - size: L

## Phase 124.3: Retirement and measurement

- [ ] M125.P3.T1 — Retire the OFX multi-frame nodes, TimeDissolve and TimeBuffer, and fix references
  - files: `Engine/OfxRetired/<ofx-plugin-id>` (one per retired ID), `Engine/EffectInstance.h`, `Engine/OfxEffectInstance.cpp`, `Engine/OfxParamInstance.cpp`
  - approach:
    - Add an `Engine/OfxRetired/<ofx-plugin-id>` marker for each of FrameBlend, TimeBlur, NoTimeBlur, AppendClip, SlitScan, TimeDissolve, TimeBufferRead and TimeBufferWrite.
    - Drop `PLUGINID_OFX_TIMEDISSOLVE`, the TimeDissolve curve default (`OfxParamInstance.cpp:4850-4854`), and the default-channel entries.
  - verify: at the phase build, the retired IDs don't load and `git grep` finds no live use of them outside the `Engine/OfxRetired/` markers.
  - size: M

- [ ] M125.P3.T2 — Repoint tests and bench comps at the native nodes
  - files: `Tests/FrameGraphBuild_Test.cpp`, `Tests/SchedulerEquivalence_Test.cpp`, `Tests/GpuResidency_Test.cpp`, `Tests/TimeVaryingLayers_Test.cpp`, `tools/bench/comps/build_comps.py`
  - approach:
    - Replace the OFX FrameBlend, TimeBlur and TimeDissolve uses with the native nodes, or with Dissolve where TimeDissolve was used.
    - Drop the TimeBuffer cases in `ColorViewsRender_Test.cpp:92-93`, plus `DefaultChannelSet_Test` and `NodeCategory_Test` entries.
    - Regenerate the `defocus_retime` comp.
  - verify: ctest passes at the phase build, and `defocus_retime.ntp` loads with the native nodes.
  - size: M

- [ ] M125.P3.T3 — Measure FrameBlend before and after on defocus_retime
  - files: `.plan/PLAN/DESIGN/2026-10-10-render-cost-profile.md` (append a section)
  - approach:
    - Profile `defocus_retime` with the native FrameBlend and TimeBlur, on CPU and GPU, on the target host, using the stripped release AppImage (per the board's build-mode rule).
    - Compare against the profile's 21 ms/Mpx, about 2253 ms baseline.
  - verify: the numbers and the region map are recorded in the design note and summarised under `## Decisions`.
  - size: M

**Verification gate:**
- In one build, ctest passes in both scheduler modes.
- GPU parity passes on RADV for all five nodes.
- The `defocus_retime` measurement is recorded.
- An Xvfb GUI check (FrameBlend and AppendClip on a sequence, dope sheet ranges) passes, with screenshots shared and OK'd before sign-off.
- An AppImage from `package.sh` is handed to the user for UAT.
- The retired IDs don't load.

## Decisions

- 2026-10-28: Phase 3 implemented (build pending; P3.T3 measurement needs the release AppImage on the host). Retired: AppendClip, FrameBlend, TimeBlur, NoTimeBlur, SlitScan, TimeDissolve (→ native Dissolve); TimeBuffer gets no marker (DEBUG-only in openfx-misc). OFX FrameBlend/TimeBlur cost seeds deleted; native seeds wait for P3.T3 measurements (PM call). AppendClip has its own dope-sheet type: clips with dissolve shading, can't be dragged, upstream keys drawn where AppendClip plays them. Gate items: regenerate defocus_retime.ntpz with build_comps.sh; Xvfb run of m125_dope_sheet_append_clip.py with screenshots.
- 2026-10-28: Phase 2 implemented (build pending). AppendClip: 64 optional inputs; firstFrame (OFX script name); blend weights differ from OFX (documented); a missing time-blend input reads as black (fades). SlitScan: channel-selectable retime map (OFX reads the first component); labels follow script names; window capped at the 64 frames nearest the current one with a warning (PM call, no OFX limit); the default 11-frame window renders on the CPU (GPU regions cap at 8 extra frames). Dope sheet: AppendClip should show its clip ranges (gate item) — next phase.
- 2026-10-28: Phase 1 implemented (build pending). Product kept as an operation (a port keeps every control); the fold starts from the oldest frame, so Product differs from OFX by one decay step when decay ≠ 0. The end frame is always read. FrameBlend's foreground-matte input becomes the standard mask. TimeBlur has no mask/mix (as OFX and the M124 nodes). Output Count and empty-input frames render on the CPU. GPU limits accepted for now (PM): FrameBlend above 8 frames falls back to the CPU because placement/regions cap views at 8 (raising it is engine work — follow-up); TimeBlur's fractional sample times stay on the CPU (regions connect whole frames only).
- 2026-10-13 — **FrameBlend, TimeBlur and NoTimeBlur stay three nodes** (user). They are not consolidated.
- 2026-10-13 — **AppendClip and SlitScan are in** (user).
- 2026-10-13 — **TimeDissolve is retired, not ported** (user): animating native Dissolve's `which` is sufficient.
- 2026-10-13 — **All time nodes on the GPU, to maximise residency** (user), on M124 - Native Time Remaps' multi-time views.
