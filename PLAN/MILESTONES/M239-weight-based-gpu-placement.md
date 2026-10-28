# M239 - Weight-Based GPU Placement

Replace the measured GPU cost model with a heuristic that needs no measurement (user, 2026-10-26). Each node declares a weight class: **Point** = 1, **Filter** = y, or **Heavy**, which asks for its region to go to the GPU. A region goes to the GPU when any member is Heavy, or when its summed weight reaches z. When transfers are free (the region's inputs are already on the device, or its root goes to the GPU viewer), any GPU-capable region goes. A minimum pixel count keeps tiny regions on the CPU. This removes the seed rows, the in-session EMA, `render-profile-report.py --emit-cost-table`, every `*.costseed.inc`, `Engine/GpuCostSeeds/` and the cost-seed step of the native node checklist. The startup transfer calibration stays. Rationale: `PLAN/DECISIONS/2026-10-26-weight-based-gpu-placement.md`.

The placement framework already takes its judgment as callbacks (`PlacementInputs::regionGain` and `isPriced`, `Engine/GpuPlacement.h:45`), wired from `GpuCostModel` in `makeRenderPlacementInputs` and `waitForGpuInitIfWorthIt` (`Engine/GpuPlacement.cpp:~800-880`). The new rule replaces those callbacks; region growth, convexity and shrinking stay as they are.

## Scope

**In:**
- A weight declaration on native nodes, defaulting from the op's `OpKind` (`Engine/Nodes/Image/ImagePipelineSpec.h:127`): Point, Composite and Route are Point; Passes and Resample are Filter; Custom is Heavy. Runtime-shader nodes are always Heavy. A node may override the default, optionally reading its knobs (for example Erode/Dilate size, FIR Blur size). The declaration replaces `gpuCostHint`/`GpuCostHint` (`Engine/Nodes/NativeEffectBase.h:81`).
- The region rule (any Heavy member, or summed weight ≥ z), the free-transfer case and the pixel floor, as constants with a comment giving their basis.
- Removal of the measured model: seed tables, EMA (`observeCpu`/`observeGpu`), `isPriced`, the per-plugin kernel estimates, the seed fragments and their CMake aggregation, `--emit-cost-table` and its self-test coverage, and the README checklist item. `GpuCostModel` shrinks to the transfer rates; rename it if a better name fits.
- Picking y, z and the floor from one directional run of the bench comps on the GPU host.

**Out:**
- Changing region growth, convexity or shrinking in `placeGraph`.
- OFX plug-in GPU rendering: M86 - OFX 1.5 GPU Suites. Until it lands, OFX plug-ins aren't GPU candidates, so they need no weight.
- A user-facing "always GPU" mode. Possible later; the existing Off/Auto preference stays.

**Edge cases:**
- In-flight node milestones (for example M184 - Variable-Size Filter Kit, M185 - Variable Warps, M123 - Native Sharpen And Glow) may still add `*.costseed.inc` files. Whichever lands second deletes them during its rebase, and CMake must no longer glob them.
- A region whose only members are identities or cached: unchanged, as now.
- GPU still initializing: `waitForGpuInitIfWorthIt` waits only when the heuristic would put some region on the GPU, still bounded by `kGpuInitWaitLimit`.
- Strip pipelining (`pipelinesTransfers`) needs a kernel-time estimate today. It must decide from bytes and weight instead, or always pipeline above `kGpuStripOverlapMinBytes`.

**Assumptions:**
- Kernel time and transfer time both scale linearly with pixel count, so the decision barely depends on resolution, apart from the fixed round trip that the floor covers.
- From the old seeds: a lone Grade at UHD costs about as much on the CPU (about 12 ms threaded) as its transfer both ways, so z > 1.

## Phase 239.1: Weights and the rule

- [x] M239.P1.T1 — Declare node weights
  - files: `Engine/GpuWeight.h` (new), `Engine/Nodes/NativeEffectBase.h`, `Engine/Nodes/Image/StagedImageEffect.h`, `Engine/Nodes/Image/PassExecutor.h`, `Engine/Nodes/Filter/EdgeDetect.cpp`
  - approach: Add `GpuWeight` (a class Point/Filter/Heavy plus an optional numeric weight for knob-aware nodes) and a virtual `gpuWeight(time, view, mipmapLevel)` on `NativeEffectBase`. The default comes from the op's `OpKind` as in Scope; runtime-shader nodes return Heavy. Replace `gpuCostHint`/`GpuCostHint` and every override of them (grep `gpuCostHint`). Nodes whose cost grows with size (Erode/Dilate, FIR Blur) return a weight that grows with their size at render scale, capped.
  - verify: the build passes, and a gtest checks the default for each `OpKind` plus the size-scaled weight of Erode at two sizes.
  - size: M

- [x] M239.P1.T2 — Place regions by weight
  - files: `Engine/GpuPlacement.h`, `Engine/GpuPlacement.cpp`, `Tests/GpuPlacement_Test.cpp`
  - approach: Replace `isPriced` and the cost-model `regionGain` with the rule: accept a region when any member is Heavy, or its summed weight ≥ `kGpuRegionMinWeight` (z), and its pixel count ≥ `kGpuRegionMinPixels`. When transfers are free (all external inputs already on the device and the root stays on the device), accept any region at or above the floor. `waitForGpuInitIfWorthIt` waits only if the rule accepts some region. Rewrite the placement tests that exercised priced/unpriced paths against the new rule: Heavy alone goes, points below z stay, points at z go, a tiny region stays, a free-transfer region goes.
  - verify: `ctest -R GpuPlacement` passes.
  - size: L

## Phase 239.2: Remove the measured model

- [x] M239.P2.T1 — Shrink GpuCostModel to transfer rates
  - files: `Engine/GpuCostModel.h`, `Engine/GpuCostModel.cpp`, `Engine/RenderScheduler.cpp`, `Engine/GpuRegionTask.cpp`
  - approach: Delete the seed tables, CPU/GPU rows, EMA and `observeCpu`/`observeGpu`, `cpuMs`/`gpuMs`/`isPriced`/`traitsOf` and the traits hint. Keep `setTransferRates`/`transferMs`/`observeTransfer` for the region reports. Make `pipelinesTransfers` decide without a kernel-time estimate: pipeline when the region moves at least `kGpuStripOverlapMinBytes` and the rates allow it. Rename the class (for example `GpuTransferModel`) if it reads better, updating its includes.
  - verify: the build passes, and `ctest -R "GpuResidency|GpuRegion|RenderScheduler"` passes.
  - size: L

- [x] M239.P2.T2 — Delete the seed fragments and generator
  - files: `Engine/Nodes/**/*.costseed.inc`, `Engine/GpuCostSeeds/`, `Engine/CMakeLists.txt`, `tools/bench/render-profile-report.py`, `Tests/CMakeLists.txt`
  - approach: Remove every `*.costseed.inc`, `Engine/GpuCostSeeds/` and the `GpuCostSeedsAll.inc` aggregation. In `render-profile-report.py`, drop `--emit-cost-table`, `GPU_KERNEL_UHD_MS` and the other measured constants, and the self-test asserts on them; keep the report itself. Adjust `RenderProfileReportSelfTest` to match.
  - verify: the build passes, `ctest -R RenderProfileReportSelfTest` passes, and `git grep -n "costseed\|GpuCostSeeds\|emit-cost-table"` is empty outside `docs/decisions/`.
  - size: M

- [x] M239.P2.T3 — Update tests and docs that used the model
  - files: `Tests/BaseTest.cpp`, `Tests/GpuResidency_Test.cpp`, `Tests/Native/NativeTransformGpu_Test.cpp`, `Tests/Native/NativeCornerPinGpu_Test.cpp`, `Tests/Native/NativeWarpGpu_Test.cpp`, `Tests/Native/ViewerRootOnDevice_Test.cpp`, `Engine/Nodes/README.md`
  - approach: Replace seed or EMA setup in these tests (for example `setGpuSeed` to force a region onto the GPU) with weights or a Heavy test node. Rewrite the README's cost-seed checklist item and the "raw nodes have no cost seed rows" paragraph to describe weights.
  - verify: these suites pass under `ctest`, and the README has no seed references.
  - size: M

## Phase 239.3: Constants and plan sweep

- [x] M239.P3.T1 — Pick y, z and the floor
  - files: `Engine/GpuWeight.h`, `Engine/GpuPlacement.h`
  - approach: On the GPU host, render the bench comps (`tools/bench/comps/run_comps.sh`: cg_multipass, defocus_retime, keying_grade), plus point-op chains of 1–6 nodes and lone Blur/Erode through `tools/bench/node_profile.sh`, with GPU auto and off on the same boot. This is a directional pass of about an hour. Choose y, z and the pixel floor so Auto is never clearly slower than Off on these, and record them as constants with a short comment giving the basis.
  - verify: a same-boot table (Auto vs Off per case, with placement) is posted on the PR.
  - size: M

- [x] M239.P3.T2 — Strike cost-seed tasks from unstarted milestones
  - files: `.plan/PLAN/MILESTONES/*.md` (todo, unclaimed rows only)
  - approach: In each todo, unclaimed milestone, strike through any task or clause that only measures or writes cost seeds, citing the decision. For claimed rows, file an inbox change-request to each owner instead.
  - verify: `grep -ln costseed .plan/PLAN/MILESTONES` lists only done milestones and struck-through text.
  - size: S

## Decisions

- 2026-10-26 — **Claimed despite file overlap (user):** M101 - Clean-Sheet Merge Node (cost seed, README checklist), M71 - RFV Render Context (`Engine/CMakeLists.txt`, `RenderScheduler.cpp`) and M64 - Tiled Rendering (`RenderScheduler.cpp`) have unfinished tasks on files this milestone touches. M239's edits there are small (one observe call, the seed glob block, the README item), so the user chose to run now and resolve them at rebase.

- 2026-10-27 — **P1.T2 wiring:**
  - A region's pixel count is its largest non-identity member image.
  - Placeholder constants until P3.T1 tunes them: z = `kGpuRegionMinWeight` = 3, `kGpuRegionMinPixels` = 256², Filter weight = 4 with size scaling capped at 32.
  - Free transfers means every external input is already on the device and no output is consumed on the CPU.
  - When placement accepts a region, the GPU-init wait now waits up to `kGpuInitWaitLimit` (10 s) instead of only the estimated gain. Init normally takes under 100 ms; P3.T1 checks the first-frame behaviour.

- 2026-10-27 — **P2.T1: the strip pipelining rule changed, so the bench must check it:** without a kernel-time estimate, a cheap point op over a large frame (for example a lone UHD Grade) now runs as pipelined strips. The old model kept it whole because strips measured slower (15–18 ms against about 10 ms whole). P3.T1's bench includes that case, and if it regresses, gate strips on summed region weight.

- 2026-10-27 — **Constants from the bench (gpu, same boot, Ryzen 7 9800X3D + Radeon AI PRO R9700, release):** a source plus two Grades (weight 3) ran about 10% slower on the GPU, three Grades broke even and six won by 20%, so z = 4. Lone Blur and Transform regions broke even or won, so the Filter weight stays at 4, and the pixel floor stays at 256². All three comps run faster on Auto: keying_grade 0.80–0.93, cg_multipass 0.84–0.86, defocus_retime 0.52–0.54. With z = 4 a lone UHD point op stays on the CPU, so the strip-pipelining concern from P2.T1 doesn't arise there. The full table is on PR #127.

- 2026-10-27 — **Rebased onto main after M184 - Variable-Size Filter Kit (gpu):** the rebase drops the earlier merge commit, so its fixes are re-applied in 3edc835b8: IBlur and IErodeDilate declare weights, and the ten `*.costseed.inc` files main's M122 - Native Colour Utilities added are deleted. The old gate run predates the rebase, and its RADV ctest exited 1 with no log, so the gate is re-run.

- 2026-10-27 — **RADV gate run after the rebase (gpu):** the debug build passes. RADV ctest has 5 failures, all inherited from main and none in code this milestone touches: NativeNoiseGpu and NativeGpuCapability (already M240.P3.T1/T2), and three "divide by A" parity failures in M122 - Native Colour Utilities's nodes (NativeColorspaceGpu, NativeColourUtilGpu `ChainRunsAsOneRegion`, NativeHueGpu HueCorrect). This milestone only deletes those nodes' seed files. The three are added to M240 - Float Trap And Denormal Audit as P3.T3. Rebased again onto M178 - Render Context Core (8a404a253), cleanly with no cost-model residue.

**Verification gate:** the full debug and release builds and ctest pass. The GPU suites pass on RADV (`NATRON_DEV_GPU=1 tools/ci/local/test.sh ctest debug`). The bench comps' same-boot Auto vs Off table shows no case where Auto is clearly slower. PR CI is green.

- 2026-10-28 — **Rebased onto main at 50ee71b60 (gpu):** main's M123 - Native Sharpen And Glow and M185 - Variable Warps brought seven more `*.costseed.inc` files and per-pass `sigma`/`recursive` hints. The seeds are deleted (ed14b2a81). Sharpen and EdgeExtend scale their weight with blur size, as Blur does. Bloom and Glow share `BloomKernels::gpuWeight`: the pyramid filters stay at the plain Filter weight, and the full-resolution filters scale with the largest blur. EdgeBlur keeps the Filter default, and ITransform and ICornerPin keep the Heavy default. These weights come from reading the code, not from a bench. The gate is re-run on the rebased head.

- 2026-10-28 — **RADV gate on the rebased head ed14b2a81 (gpu, debug):** 590 of 597 suites pass, including every GpuPlacement, GpuResidency and GpuRegion suite. The seven failures are all inherited from main and owned by M240 - Float Trap And Denormal Audit (EdgeAware, Colorspace, ColourUtil, GpuCapability, Hue, Merge and Noise parity). The same set fails on the M196 - Copy-Compute Contention lane, which is also based on main. The Auto-vs-Off bench isn't re-run: the rebase only gave weights to nodes the bench comps don't use.
