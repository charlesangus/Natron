# M628 - Texture Budget For Small Devices

The automatic GPU texture budget refuses every file texture on software GL and on small GPUs. `Engine/Nodes/Scene/GpuTextureBudget.cpp` (`limitLocked` ~:76-83, `automaticGpuTextureHeadroom` ~:464) holds back headroom of `max(1 GiB, total/16)` from the reported free VRAM. llvmpipe reports ~32 MB free, so the limit becomes 0, and USDRender and the viewer draw untextured. The PM saw this in gdb on an 85 KB `layer.exr` during M312 - USDLayerMaterial Node. Make the automatic budget scale with what the device reports, so a small device still loads textures up to what it can hold.

## Scope

**In:**
- The automatic budget's headroom and limit rule, and how a software or unknown device is treated.

**Out:**
- The explicit (user-set) budget path and its preference.
- Texture streaming or mip-level selection.

**Edge cases:**
- **Software GL (llvmpipe):** reported free memory is not meaningful, so treat it as unknown and use a fixed default budget (a few hundred MB of host memory).
- **Free query unavailable (0):** keeps the current total/2 rule.
- **Tiny real GPU (e.g. 2 GB):** headroom scales with total, never more than a fraction of it, so the limit stays positive.

## Phase 628.1: Scale the automatic budget

- [ ] M628.P1.T1 — Make the headroom proportional and treat software GL as unknown free memory
  - files: `Engine/Nodes/Scene/GpuTextureBudget.{h,cpp}` (`automaticGpuTextureHeadroom`, `limitLocked`, the free-VRAM query); `Tests/Scene/` budget test (find the existing GpuTextureBudget test, or add `GpuTextureBudget_Test.cpp`)
  - approach: replace the 1 GiB floor with a headroom that is a fraction of total (e.g. total/16, capped at 1 GiB), so the limit never drops to 0 while free > headroom. Detect a software renderer from the GL renderer string (llvmpipe/softpipe/swrast) and use a fixed default budget instead of the free query. Keep the computation a pure function of (total, free, isSoftware) so it can be unit-tested without a GL context. Bundled gtest is old: no `GTEST_SKIP`, use `INSTANTIATE_TEST_CASE_P`.
  - verify: new unit tests cover llvmpipe-like (32 MB free, software), 2 GB GPU, 24 GB GPU and free=0 inputs, each with a positive limit; `ctest -R GpuTextureBudget` green
  - size: M
- [ ] M628.P1.T2 — Prove a file texture loads on llvmpipe
  - files: an existing USDRender or USDLayerMaterial textured-render test in `Tests/Scene/` (one that uses a small EXR texture)
  - approach: add or extend a test that renders a textured prim under the test's software GL context and checks a texel colour, not just "rendered". It runs on the container's llvmpipe, which is where the bug shows.
  - verify: the test fails with T1's change reverted locally and passes with it
  - size: M

**Verification gate:** full build and ctest green; the textured llvmpipe render shows the texture; Xvfb GUI launch of a textured USD scene in the viewer shows it textured (screenshot to the user).
