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

- [ ] M239.P1.T1 — Declare node weights
  - files: `Engine/GpuWeight.h` (new), `Engine/Nodes/NativeEffectBase.h`, `Engine/Nodes/Image/StagedImageEffect.h`, `Engine/Nodes/Image/PassExecutor.h`, `Engine/Nodes/Filter/EdgeDetect.cpp`
  - approach: Add `GpuWeight` (a class Point/Filter/Heavy plus an optional numeric weight for knob-aware nodes) and a virtual `gpuWeight(time, view, mipmapLevel)` on `NativeEffectBase`. The default comes from the op's `OpKind` as in Scope; runtime-shader nodes return Heavy. Replace `gpuCostHint`/`GpuCostHint` and every override of them (grep `gpuCostHint`). Nodes whose cost grows with size (Erode/Dilate, FIR Blur) return a weight that grows with their size at render scale, capped.
  - verify: the build passes, and a gtest checks the default for each `OpKind` plus the size-scaled weight of Erode at two sizes.
  - size: M

- [ ] M239.P1.T2 — Place regions by weight
  - files: `Engine/GpuPlacement.h`, `Engine/GpuPlacement.cpp`, `Tests/GpuPlacement_Test.cpp`
  - approach: Replace `isPriced` and the cost-model `regionGain` with the rule: accept a region when any member is Heavy, or its summed weight ≥ `kGpuRegionMinWeight` (z), and its pixel count ≥ `kGpuRegionMinPixels`. When transfers are free (all external inputs already on the device and the root stays on the device), accept any region at or above the floor. `waitForGpuInitIfWorthIt` waits only if the rule accepts some region. Rewrite the placement tests that exercised priced/unpriced paths against the new rule: Heavy alone goes, points below z stay, points at z go, a tiny region stays, a free-transfer region goes.
  - verify: `ctest -R GpuPlacement` passes.
  - size: L

## Phase 239.2: Remove the measured model

- [ ] M239.P2.T1 — Shrink GpuCostModel to transfer rates
  - files: `Engine/GpuCostModel.h`, `Engine/GpuCostModel.cpp`, `Engine/RenderScheduler.cpp`, `Engine/GpuRegionTask.cpp`
  - approach: Delete the seed tables, CPU/GPU rows, EMA and `observeCpu`/`observeGpu`, `cpuMs`/`gpuMs`/`isPriced`/`traitsOf` and the traits hint. Keep `setTransferRates`/`transferMs`/`observeTransfer` for the region reports. Make `pipelinesTransfers` decide without a kernel-time estimate: pipeline when the region moves at least `kGpuStripOverlapMinBytes` and the rates allow it. Rename the class (for example `GpuTransferModel`) if it reads better, updating its includes.
  - verify: the build passes, and `ctest -R "GpuResidency|GpuRegion|RenderScheduler"` passes.
  - size: L

- [ ] M239.P2.T2 — Delete the seed fragments and generator
  - files: `Engine/Nodes/**/*.costseed.inc`, `Engine/GpuCostSeeds/`, `Engine/CMakeLists.txt`, `tools/bench/render-profile-report.py`, `Tests/CMakeLists.txt`
  - approach: Remove every `*.costseed.inc`, `Engine/GpuCostSeeds/` and the `GpuCostSeedsAll.inc` aggregation. In `render-profile-report.py`, drop `--emit-cost-table`, `GPU_KERNEL_UHD_MS` and the other measured constants, and the self-test asserts on them; keep the report itself. Adjust `RenderProfileReportSelfTest` to match.
  - verify: the build passes, `ctest -R RenderProfileReportSelfTest` passes, and `git grep -n "costseed\|GpuCostSeeds\|emit-cost-table"` is empty outside `docs/decisions/`.
  - size: M

- [ ] M239.P2.T3 — Update tests and docs that used the model
  - files: `Tests/BaseTest.cpp`, `Tests/GpuResidency_Test.cpp`, `Tests/Native/NativeTransformGpu_Test.cpp`, `Tests/Native/NativeCornerPinGpu_Test.cpp`, `Tests/Native/NativeWarpGpu_Test.cpp`, `Tests/Native/ViewerRootOnDevice_Test.cpp`, `Engine/Nodes/README.md`
  - approach: Replace seed or EMA setup in these tests (for example `setGpuSeed` to force a region onto the GPU) with weights or a Heavy test node. Rewrite the README's cost-seed checklist item and the "raw nodes have no cost seed rows" paragraph to describe weights.
  - verify: these suites pass under `ctest`, and the README has no seed references.
  - size: M

## Phase 239.3: Constants and plan sweep

- [ ] M239.P3.T1 — Pick y, z and the floor
  - files: `Engine/GpuWeight.h`, `Engine/GpuPlacement.h`
  - approach: On the GPU host, render the bench comps (`tools/bench/comps/run_comps.sh`: cg_multipass, defocus_retime, keying_grade), plus point-op chains of 1–6 nodes and lone Blur/Erode through `tools/bench/node_profile.sh`, with GPU auto and off on the same boot. This is a directional pass of about an hour. Choose y, z and the pixel floor so Auto is never clearly slower than Off on these, and record them as constants with a short comment giving the basis.
  - verify: a same-boot table (Auto vs Off per case, with placement) is posted on the PR.
  - size: M

- [ ] M239.P3.T2 — Strike cost-seed tasks from unstarted milestones
  - files: `.plan/PLAN/MILESTONES/*.md` (todo, unclaimed rows only)
  - approach: In each todo, unclaimed milestone, strike through any task or clause that only measures or writes cost seeds, citing the decision. For claimed rows, file an inbox change-request to each owner instead.
  - verify: `grep -ln costseed .plan/PLAN/MILESTONES` lists only done milestones and struck-through text.
  - size: S

## Decisions

- 2026-10-26 — **Claimed despite file overlap (user):** M101 - Clean-Sheet Merge Node (cost seed, README checklist), M71 - RFV Render Context (`Engine/CMakeLists.txt`, `RenderScheduler.cpp`) and M64 - Tiled Rendering (`RenderScheduler.cpp`) have unfinished tasks on files this milestone touches. M239's edits there are small (one observe call, the seed glob block, the README item), so the user chose to run now and resolve them at rebase.

**Verification gate:** the full debug and release builds and ctest pass. The GPU suites pass on RADV (`NATRON_DEV_GPU=1 tools/ci/local/test.sh ctest debug`). The bench comps' same-boot Auto vs Off table shows no case where Auto is clearly slower. PR CI is green.
