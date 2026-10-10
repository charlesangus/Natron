# GPU residency from day one

2026-10-10 (user), after reviewing M82 - Render Cost Profiling's comp report (`PLAN/DESIGN/2026-10-10-render-cost-profile.md`): go for the GPU path, and M84 - GPU Placement And Residency builds resident GPU regions from the start instead of placing isolated heavy nodes first. In every profiled comp the heavy nodes joined into one region per frame. Chains of cheap point ops (Grade, Merge) only pay off on the GPU when they stay resident; this lifts the CG multi-pass comp's estimated gain from ~25% to ~70%. Staging vs DMA moved the results by under 4 points.
