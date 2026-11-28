## Pending

### 2026-11-27 — change-request — Roto render: still-frame and edit speedups (M299 follow-up)
- from: bosley-executor (cat-pm)
- refs: M299
- body: M299 - Roto Render Performance shipped below two of its targets (user accepted): 100-shape still UHD later frames 2.2-2.4x vs the 3x target, and knob edits (edit-colour, edit-grade) 1.2-1.4x vs 1.5x. Proposed milestone "Roto Still Frame And Edit Speed": profile the 100-shape still UHD later frame and the edit cases on M299's build (Merge compositing and RGBA conversion were the biggest costs in the M299 profile), cut the next largest cost, re-bench with tools/bench/roto_render_bench.sh on a cloud VM. Also: the roto GUI stroke bench (`tools/bench/gui/run.sh --host --driver roto --ops stroke_5px,stroke_50px,stroke_300px,pen_up`) ran 40+ min with no records on an RTX 3060 VM for both main and M299 — make it finish within the bench budget (likely the 300-node upstream comp build) so stroke latency can be gated.

### 2026-11-27 — change-request — Delegate light-level mismatches outside Cycles (from M365)
- from: bosley-executor (cat-pm)
- refs: M365
- body: Found while matching Cycles light levels to Storm/MoonRay in M365 - Delegate Display Transform (UsdLux used as the reference): (1) MoonRay's *normalized* distant light renders π brighter than UsdLux and Storm (unnormalized all agree) — hdMoonray fork translation; (2) Storm treats a normalized sphere light as a point light, 4× UsdLux; (3) hdCycles doesn't scale the sphere light radius by metersPerUnit, so unnormalized sphere lights are the wrong size in cm stages. Proposed small milestone "Delegate Light Unit Parity": fix each in its fork / Storm path with a per-light-type level test like Tests/Scene/CyclesLightLevels_Test.cpp.
