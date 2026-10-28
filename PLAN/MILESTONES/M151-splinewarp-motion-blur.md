# M151 - SplineWarp Motion Blur

Motion blur for SplineWarp from animated curves, correspondences and amounts. Each sub-frame time gets its own solved field, and the shutter samples are averaged, on the CPU and the GPU.

Deps:
- M147 - SplineWarp Node Core (the field and its time-keyed cache)
- M150 - SplineWarp GPU Kernel (the GPU path that this extends)

Scouted facts (2026-10-20, `main` at `8fdb5741f`):
- The shared motion-blur knob names and options are in `Engine/Nodes/Image/Resampler.h`: `kResamplerParamShutterOffset` (`:54`), `kResamplerParamShutterCustomOffset` (`:57`), and the shutterOffset choice (`:100`).
- `BlurSettings` (`:391`), `buildSamplingTransforms` (`:445`) and `motionBlurPixel` drive Transform's matrix-only blur.

## Scope

**In:**
- Knobs: `motionBlur` (the sample count, 0 = off), `shutter`, `shutterOffset`, `shutterCustomOffset`, with Resampler's names and options.
- CPU: one field per sub-frame time (cached), stratified averaging, and an RoD/RoI union across sub-frames.
- GPU: sub-frames accumulated through M150 - SplineWarp GPU Kernel's kernel, with a parity test.

**Out:**
- Motion blur from upstream transform animation separately from the curves → none. The concatenated matrix is re-evaluated per sub-frame, so it is already included.

**Edge cases:**
- Nothing animated across the shutter: skip the sub-frames, and render once.
- Shutter 0 or motionBlur 0: identical to no blur, bit-exact.
- The sub-frame field cache must not evict the main-frame field during one render: bound it per render.

## Phase 151.1: Motion blur

- [ ] M151.P1.T1 — CPU motion blur
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarpField.cpp`, `Tests/Native/NativeSplineWarpMotionBlur_Test.cpp` (new)
  - approach:
    - Add the knobs, and compute the sub-frame times as Transform does.
    - Per sub-frame, build or fetch the field (re-evaluating the concatenated matrix at that time), resample into an accumulator, and average.
    - RoD and RoI are the union over the sub-frames.
    - Skip the sub-frames when the curve hash is constant across the shutter.
  - verify: the test covers a link translating linearly over the shutter matching Transform's motion blur of the same translation (1e-3), shutter 0 bit-exact, and the static-shutter skip (render count).
  - size: L

- [ ] M151.P1.T2 — GPU motion blur
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Engine/Gpu/Kernels/splinewarp.slang`, `Tests/Native/NativeSplineWarpGpu_Test.cpp`
  - approach: upload each sub-frame's field and dispatch accumulate-mode passes (or one pass over a field array if the executor allows it), then normalise. Remove the motion-blur decline from M150 - SplineWarp GPU Kernel.
  - verify: GPU parity with the CPU path on P1.T1's fixture within 1e-5. It runs on RADV.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including the GPU leg on RADV.
- Xvfb GUI check, with a screenshot shared before sign-off: a fast-moving link with motion blur on.
- An AppImage built with `package.sh` for UAT.

## Decisions

- 2026-10-28: Implemented (build pending). Knobs motionBlur (0, 0–100), shutter (0.5), shutterOffset (Start), shutterCustomOffset (0), following CornerPin (Nuke's SplineWarp has no motion-blur controls). Sub-frames = min(64, max(13, int(40·motionBlur)/3)) at stratum centres, equal weights (Transform samples per pixel randomly). Static shutter renders once; RoD/RoI are unions over sub-frames; identity only if all are. Separate 64-entry sub-frame field cache. Motion blur stays on in draft. PM call: the upstream concatenated matrix is evaluated once at frame time (the concatenation API supplies one matrix per render), as Transform and CornerPin do — deviation from the milestone's Out note. GPU averages all sub-frames in one dispatch.
- 2026-10-20 — **Motion blur is in scope (user).**
