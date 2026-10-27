# M124 - Native Time Remaps

Four Nuke-style native time nodes replace the openfx-misc ones: `TimeOffset`, `FrameHold`, `FrameRange` and `Retime`. They are redesigned rather than ported, with plain IDs at major 1, Nuke-like knobs and no backward compatibility. Three of them only change which frame the input is read at, so they are identity at another time and cost no pixel work. Retime's blend filter is the one that mixes two frames.

This milestone also owns the **multi-time infrastructure** that both time milestones need:
- native nodes fetching their input at times other than the render time;
- a GPU interface that can supply one input at several times;
- placement that lets identity-at-another-time nodes pass through a resident GPU region instead of breaking it.

M125 - Native Multi-Frame Nodes builds on all three.

Deps: M120 - Native Node Port Kit (per-node registration, kernel and doc fragments; per-milestone OFX retirement list).

Scouted facts (scouted 2026-10-13 on main `338d9324a`; M85 - Native GPU Kernels facts from `origin/milestone/m85-native-gpu-kernels`; re-check at promotion):
- **OFX sources** (`build/assets/plugin-src/openfx-misc/`):
  - **TimeOffset** (`TimeOffset/TimeOffset.cpp`, `net.sf.openfx.timeOffset`):
    - Knobs: `timeOffset` (int 0, not animating), `reverseInput`, `clipToInputRange`.
    - `getSourceTime` (`:123`) computes `t - offset`, optionally reversed (`max - s + min`) and clamped to the input range.
    - `isIdentity` is always true at that time.
    - `getTimeDomain` is the input range shifted by the offset.
  - **FrameHold** (`FrameHold/FrameHold.cpp`):
    - Knobs: `firstFrame` (int 0) and `increment` (int 0).
    - The source time is `firstFrame + inc*floor((t-first)/inc)`, or just `firstFrame` when the increment is 0.
    - Always identity.
  - **FrameRange** (`FrameRange/FrameRange.cpp`):
    - Knobs: `frameRange` (int2, default 1,1) and `before`/`after` (Original, Hold, Loop, Bounce, Black; default Black).
    - `isIdentity` (`:274`) maps out-of-range times; Black renders empty.
    - `getTimeDomain` is at `:445`.
  - **Retime** (`Retime/Retime.cpp`):
    - Knobs: `speed` (1), `reverseInput`, `filter` (None, Nearest, Linear; default Linear), a `warp` parametric curve, and `duration`.
    - The source time is `min + ∫speed` (`:331, :398`), or `min + r*warp((s-min)/r)` with the warp curve.
    - Linear blends `floor(s)` and `floor(s)+1`.
- **Engine and GUI references:**
  - **ID defines:** `PLUGINID_OFX_TIMEOFFSET/FRAMEHOLD/RETIME/FRAMERANGE/TIMEDISSOLVE` at `Engine/EffectInstance.h:66-71`.
  - **Roto clone tool:** creates TimeOffset and FrameHold nodes (`Engine/RotoDrawableItem.cpp:231-251`) and drives `timeOffset`/`firstFrame` by name (`:570-572, :1009-1011`). Knob-name defines are at `RotoDrawableItem.cpp:68-69`, `RotoItem.cpp:59-60` and `RotoContext.cpp:75-76`.
  - **Dope sheet:**
    - `Gui/DopeSheet.cpp:322-327` maps the three IDs to item types. `:205-207` finds the nearest downstream time node, and `:1586` treats types from Retime up as time nodes.
    - `Gui/DopeSheetHierarchyView.cpp:244-246` handles the same types.
    - `Gui/DopeSheetView.cpp` has `computeRetimeRange` (`:1937`, calls `getFramesNeeded`), `computeTimeOffsetRange` (`:2018`), `computeFRRange` (`:2056`), and the move cursors at `:517-518`.
    - `Gui/DopeSheetEditorUndoRedo.cpp:75-112` moves TimeOffset and FrameRange by knob name. The knob-name constants are at `Gui/DopeSheet.h:48-52`.
  - **Curve default:** `Engine/OfxParamInstance.cpp:4850-4854` sets the Retime warp curve's default interpolation to Cubic.
  - **Other ID lists:** `Engine/OfxEffectInstance.cpp:1298-1309, 1338`.
- **Native infrastructure:**
  - **Time virtuals** are on `EffectInstance`, not `NativeEffectBase`:
    - `isIdentity(time, …, &inputTime, &inputView, &inputNb)` (`Engine/EffectInstance.h:1436`);
    - `getFramesNeeded` (`:1541`), whose default asks each input for `[time,time]` (`EffectInstance.cpp:1740`);
    - `getFrameRange` (`:1549`).
  - **`NativeImageEffect::isIdentity`** (`NativeImageEffect.cpp:344`) hard-codes `*inputTime = time` and input 0.
  - **No native node fetches an image at another time.** Every `getImage(` call in `Engine/Nodes` uses the render time. `Engine/Nodes/README.md:669-671` requires extra times to be declared through `getFramesNeeded()`.
  - **Closest model:** `Tests/FrameGraphBuild_Test.cpp:514-548`, which covers the FrameBlend + TimeOffset pre-fetch cap.
  - **`ReadTimeDomain`** (`Engine/Nodes/IO/ReadTimeDomain.h`): `Settings`, `outputRange()` and `resolve(time, settings, framesOnDisk)` with Hold, Loop, Bounce, Black and Error. It is used by `NativeRead.cpp:249-338, 656-658, 813`.
  - **GPU interface:**
    - `Engine/Nodes/Image/GpuRender.h` defines `GpuQuery`, `GpuImageView` and `GpuRenderArgs`. `GpuRenderArgs::inputs` is indexed by input number, with one view per input at the render time.
    - Placement and regions live in `Engine/GpuPlacement*` and `Engine/GpuRegionTask*`.
    - The M85 - Native GPU Kernels point-op description is `PixelKernelGpu` with `makeGpuKernel` (`PixelKernel.h:141`, `NativeImageEffect.h:214`, on the M85 - Native GPU Kernels branch).

## Scope

**In:**
- Native `TimeOffset`, `FrameHold`, `FrameRange` and `Retime` with plain IDs at major 1.
- A base-class hook for identity at another time.
- Native multi-time image fetch through `getFramesNeeded` + `getImage(time)`.
- GPU multi-time input views, and placement that keeps a resident region through identity-at-another-time nodes.
- A GPU kernel for Retime's blend filter.
- The dope sheet, the roto clone tool and the curve default moved to the native IDs and knobs.
- The four OFX IDs retired.

**Out:**
- FrameBlend, TimeBlur, NoTimeBlur, AppendClip and SlitScan: M125 - Native Multi-Frame Nodes.
- TimeDissolve, which is retired in M125 - Native Multi-Frame Nodes.
- Optical-flow retiming (Kronos/OFlow-style): none.
- Renaming the existing native nodes' IDs: M111 - Native Node Identity Cleanup.

**Edge cases:**
- **FrameRange set to Black outside the range:** renders an empty frame, not identity. On the GPU this is a zero fill inside the region.
- **Retime at an integral source time, or with the Nearest filter:** identity at that time, with no blend.
- **Retime reversed with a warp curve:** the warp applies after the reversal, as in the OFX node.
- **TimeOffset with the input range clamp on:** times outside the input range clamp to the first or last frame.
- **A time remap upstream of a GPU region:** the region requests the input at the remapped time. It must not fall back to the CPU just because the time differs.

**Assumptions:**
- Unconfirmed: the knobs are Nuke's.
  - `TimeOffset`: `time_offset`, `reverse_input`, `clamp`.
  - `FrameHold`: `first_frame`, `increment`.
  - `FrameRange`: `first_frame`, `last_frame`, `before`, `after`.
  - `Retime`: `input.first`/`input.last`, `output.first`/`output.last`, `speed`, `reverse`, `filter`, plus a `timewarp` curve.
- Unconfirmed: Retime's filter options are Nearest and Blend (two frames). OFX's "None", which passes a fractional time upstream, is kept as "Fractional" for motion-blur-aware inputs.
- Unconfirmed: multi-time GPU inputs are views keyed by (input, time) and capped at a small fixed count per dispatch. M125 - Native Multi-Frame Nodes raises the cap if FrameBlend needs it.

## Phase 123.1: Multi-time foundation

- [ ] M124.P1.T1 — Add a time-remap hook to NativeImageEffect's identity path
  - files: `Engine/Nodes/Image/NativeImageEffect.{h,cpp}`, `Tests/Native/NativeImageEffect_Test.cpp`
  - approach:
    - Add a virtual `remapInputTime(time, view, &inputTime) -> bool`, defaulting to "no remap". `isIdentity` uses it to report identity at another time before the mix/mask rules apply.
    - Add a matching `getFramesNeeded` default that asks for the remapped time.
    - Nodes that never override it must behave exactly as before.
  - verify: a test node offsetting time by +3 reports identity at `t+3`, and `getFramesNeeded` lists `t+3`. Existing native tests pass at the phase build.
  - size: M

- [ ] M124.P1.T2 — Let native render code fetch an input at another time
  - files: `Engine/Nodes/Image/NativeImageEffect.{h,cpp}`, `Engine/Nodes/README.md`, `Tests/Native/NativeMultiTime_Test.cpp` (new)
  - approach:
    - Add a helper `fetchInputAt(inputNb, time, …)` that only fetches times the node declared in `getFramesNeeded`, and fails loudly in tests otherwise.
    - Model it on the FrameGraphBuild pre-fetch path (`Tests/FrameGraphBuild_Test.cpp:514-548`) so the task-graph scheduler sees every dependency.
    - Document the rule in the README.
  - verify: a test node averaging `t-1` and `t+1` renders the right pixels under both scheduler modes, and an undeclared time is rejected. Runs at the phase build.
  - size: L

- [ ] M124.P1.T3 — Extend the GPU interface to supply an input at several times
  - files: `Engine/Nodes/Image/GpuRender.h`, `Engine/GpuRegionTask.{h,cpp}`, `Engine/GpuPlacement.{h,cpp}`, `Tests/GpuResidency_Test.cpp`
  - approach:
    - Key `GpuRenderArgs` input views by (input, time). Single-time nodes keep `inputs[i]` as the render-time view.
    - `GpuQuery` carries the times a node needs per input, taken from `getFramesNeeded`.
    - Placement imports or keeps resident every (input, time) view the region needs, up to a fixed per-dispatch cap.
  - verify: `GpuResidency_Test` covers a two-time consumer whose views come from a resident producer rendered at two times. It runs at the phase build on lavapipe/RADV.
  - size: L

- [ ] M124.P1.T4 — Keep resident regions through identity-at-another-time nodes
  - files: `Engine/GpuPlacement.{h,cpp}`, `Engine/GpuRegionTask.cpp`, `Tests/GpuPlacement_Test.cpp`
  - approach:
    - When a member reports identity at another time, placement re-targets the downstream request to the producer at that time. Do not end the region there.
    - Count the remap as a member with no kernel.
  - verify: `GpuPlacement_Test` puts Grade → TimeOffset test node → Grade in one region, and the profile shows no download between them. Runs at the phase build.
  - size: M

## Phase 123.2: Nodes

- [ ] M124.P2.T1 — Native TimeOffset and FrameHold
  - files: `Engine/Nodes/Time/TimeOffset.{h,cpp}` (new), `Engine/Nodes/Time/FrameHold.{h,cpp}` (new), `Engine/Nodes/Time/{FrameHold,TimeOffset}.reg.inc` (new), `Tests/Native/NativeTimeOffset_Test.cpp` (new)
  - approach:
    - Both are pure remaps through P1.T1's hook, with Nuke knobs.
    - TimeOffset has `time_offset`, `reverse_input` and `clamp`, and its `getFrameRange` shifts the input range.
    - FrameHold has `first_frame` and `increment`; a sublabel shows the held frame.
    - Register both through the port kit's per-node `.reg.inc` fragments.
  - verify: tests cover the offset, reverse, clamp, the hold with and without an increment, and frame-range reporting. Runs at the phase build.
  - size: M

- [ ] M124.P2.T2 — Native FrameRange on ReadTimeDomain
  - files: `Engine/Nodes/Time/FrameRange.{h,cpp}` (new), `Engine/Nodes/IO/ReadTimeDomain.h` (only if a shared helper needs exposing), `Engine/Nodes/Time/FrameRange.reg.inc`, `Tests/Native/NativeFrameRange_Test.cpp` (new)
  - approach:
    - `first_frame`/`last_frame` and `before`/`after` (Hold, Loop, Bounce, Black), resolved through `ReadTimeDomain::resolve`.
    - Black renders empty, with a GPU zero-fill path through the description.
    - `getFrameRange` reports the set range.
  - verify: tests cover each before/after mode at frames inside, before and after the range. Runs at the phase build.
  - size: M

- [ ] M124.P2.T3 — Native Retime with speed, reverse and timewarp curve
  - files: `Engine/Nodes/Time/Retime.{h,cpp}` (new), `Engine/Nodes/Time/Retime.reg.inc`, `Tests/Native/NativeRetime_Test.cpp` (new)
  - approach:
    - Input and output ranges, `speed`, `reverse`, and an optional `timewarp` parametric curve with cubic default keys. Port the source-time integration from OFX `Retime.cpp:331, 398`.
    - Filter options are Nearest (identity at the rounded time), Blend (fetch `floor(s)` and `floor(s)+1` via P1.T2, then lerp) and Fractional (pass the fractional time through).
  - verify: tests cover speed 0.5 and 2, reverse, a warp curve, and each filter at integral and fractional times. Runs at the phase build.
  - size: L

- [ ] M124.P2.T4 — Retime's blend on the GPU
  - files: `Engine/Gpu/Kernels/timeblend.slang` (new), `Engine/Gpu/Kernels/timeblend.cmake` (new), `Engine/Nodes/Time/Retime.cpp`, `Tests/Native/NativeRetimeGpu_Test.cpp` (new)
  - approach:
    - A two-view weighted blend kernel on P1.T3's multi-time views. It is written generically (N views with weights) so M125 - Native Multi-Frame Nodes reuses it.
    - Retime's `makeGpuKernel` fills the views and weights.
  - verify: the GPU parity harness matches the CPU Blend within 1e-6 absolute at several fractional times. Runs at the phase build on RADV.
  - size: M

## Phase 123.3: Callers and retirement

- [ ] M124.P3.T1 — Move the dope sheet to the native time nodes
  - files: `Gui/DopeSheet.cpp`, `Gui/DopeSheet.h`, `Gui/DopeSheetHierarchyView.cpp`, `Gui/DopeSheetView.cpp`, `Gui/DopeSheetEditorUndoRedo.cpp`
  - approach:
    - Map the native plain IDs to the Retime, TimeOffset and FrameRange item types (`DopeSheet.cpp:322-327`), and keep the nearest-time-node and type-range logic (`:205-207, :1586`).
    - Update the knob-name constants (`DopeSheet.h:48-52`) and the range computations and moves (`DopeSheetView.cpp:1937-2056, 517-518`; `DopeSheetEditorUndoRedo.cpp:75-112`) to the new knob names.
  - verify: an Xvfb GUI check at the phase build: a Read → TimeOffset → FrameRange → Retime chain shows correct clip ranges in the dope sheet, and dragging the TimeOffset and FrameRange clips changes their knobs. Take screenshots.
  - size: M

- [ ] M124.P3.T2 — Move the roto clone tool and the ID defines to the native nodes
  - files: `Engine/RotoDrawableItem.cpp`, `Engine/RotoItem.cpp`, `Engine/RotoContext.cpp`, `Engine/EffectInstance.h`, `Engine/OfxParamInstance.cpp`
  - approach:
    - The clone tool's internal TimeOffset and FrameHold use the native IDs and knob names (`RotoDrawableItem.cpp:68-69, 231-251, 570-572, 1009-1011`; `RotoItem.cpp:59-60`; `RotoContext.cpp:75-76`).
    - Replace `PLUGINID_OFX_TIMEOFFSET/FRAMEHOLD/RETIME/FRAMERANGE` (`EffectInstance.h:66-71`) with the native macros.
    - Drop the Retime branch of the OFX curve-default code (`OfxParamInstance.cpp:4850-4854`); the native Retime sets its own default.
  - verify: the existing roto tests pass at the phase build. Manual check: a clone stroke with a time offset renders from the offset frame.
  - size: M

- [ ] M124.P3.T3 — Retire the OFX time remaps and fix references
  - files: `Engine/OfxRetired/<ofx-plugin-id>` (new, one per retired ID), `Engine/OfxEffectInstance.cpp`, `Tests/DefaultChannelSet_Test.cpp`, `Tests/NodeCategory_Test.cpp`
  - approach:
    - List `net.sf.openfx.timeOffset`, `FrameHold`, `FrameRange` and `Retime` as `Engine/OfxRetired/<ofx-plugin-id>` markers, one per ID.
    - Remove their entries from the OFX default-channel lists (`OfxEffectInstance.cpp:1298-1309`).
    - Repoint tests that used them, including `FrameGraphBuild_Test.cpp:74, 514-548` (the `FrameGraphBuild` case moves to native TimeOffset; keep FrameBlend on OFX until M125 - Native Multi-Frame Nodes).
  - verify: at the phase build, the retired IDs no longer load, `git grep` finds no live use of them outside the `Engine/OfxRetired/` markers, and ctest passes.
  - size: M

**Verification gate:**
- In one build, ctest passes in both scheduler modes.
- GPU parity for Retime's blend passes on RADV, and `GpuPlacement_Test` shows a region spanning a TimeOffset.
- The Xvfb GUI check of the dope sheet and roto clone passes, with screenshots shared and OK'd before sign-off.
- An AppImage from `package.sh` is handed to the user for UAT.
- The four retired OFX IDs don't load.

## Decisions

- 2026-10-27 — Retime's Output First/Last are derived from Speed while their locks are off and reset on edit, as in Nuke; the hash sweep exempts them (allowlist) and `NativeRetimeTest.LockedOutputEndsMoveTheHash` covers the locked case. FrameRange's Black outside the range is an empty render (no plane), not a black plane.
- 2026-10-13 — **Four Nuke-style nodes** (user): TimeOffset, FrameHold, FrameRange and Retime stay separate. They are not merged into one Time node or into each other.
- 2026-10-13 — **All time nodes on the GPU, to maximise residency** (user). For the pure remaps this means placement keeps a region through them, since they have no pixel work. Retime's blend gets a kernel.
- 2026-10-13 — **This milestone owns the multi-time infrastructure:** the native fetch, the GPU (input, time) views and the placement pass-through. M125 - Native Multi-Frame Nodes depends on it, so the remaps, which are the simplest consumers, prove it first.
