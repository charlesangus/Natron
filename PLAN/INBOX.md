## Pending

### 2026-10-26T23:00:00Z — change-request
- refs: M101
- From gpu (user decision, 2026-10-26): cost seeds are void (`PLAN/DECISIONS/2026-10-26-weight-based-gpu-placement.md`). Strike any unfinished M101 - Clean-Sheet Merge Node task or clause that measures or writes a `*.costseed.inc`, and don't add the README cost-seed checklist step. M239 - Weight-Based GPU Placement deletes the seed machinery; if M101 lands first, its already-committed seed file is removed in M239's rebase.

### 2026-10-27T12:00:00Z — change-request
- refs: M123, M185
- From cat-plan, re-filed from the 2026-10-27T05:00:00Z entry (M122 - Native Colour Utilities and M129 - Clean-Sheet Noise Node are done, so only these two remain). Cost seeds are abolished (`PLAN/DECISIONS/2026-10-26-weight-based-gpu-placement.md`). M239 - Weight-Based GPU Placement deletes `*.costseed.inc`, `Engine/GpuCostSeeds/`, `render-profile-report.py --emit-cost-table` and the measured cost model, and nodes declare `gpuWeight()` instead (Engine/GpuWeight.h). Override it only when cost grows with a knob (`gpuSizeScaledWeight`) or the work always wants the GPU (`GpuWeight::heavy()`). In M123 - Native Sharpen And Glow and M185 - Variable Warps, strike any unfinished task or clause that measures or writes cost seeds, and don't add new `*.costseed.inc` files. Seed files already on a branch are deleted when that branch rebases after M239 merges, or by M239's rebase if your milestone merges first.

### 2026-10-27T05:18:45+00:00 — change-request
- refs: M207
- Use OCIO to do the compression; can use an srgb display-referred space that compresses to 0-1 to go from most colourspaces to srgb, and it's invertible.

### 2026-10-27T05:19:47+00:00 — change-request
- refs: M203
- Spike/investigate if we _need_ to end GPU regions for this to be practical.

### 2026-10-27T05:22:05+00:00 — change-request
- refs: M187
- Hold on this til my say-so.

### 2026-10-27T05:22:20+00:00 — change-request
- refs: M188
- Hold on this until my say-so.
