# M651 - MoonRay Live Volume Refresh

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Found by M648 - Cycles Live Volume Refresh's live-field test (m648-gate1, run 46114401928): on MoonRay, editing a VDB field prim on a live engine logs "HdChangeTracker::MarkRprimDirty: Calling method on HdChangeTracker that requires emulation" from hdMoonray's field sync, and the render after the edit carries a volume warning. USD 26.08's scene-index Hydra already forwards field→volume dependencies, so the delegate's own dirtying is unneeded there (the Cycles fix removed it).

Acceptance sketch:
- `VolumeRenderMoonRay.ALiveFieldEditMovesTheVolume` (committed DISABLED_ by M648) is re-enabled and passes: no emulation coding error, no volume warning, the volume moves, no engine remake.
- The fix lands in the hdMoonray fork on a branch off F1's pin and is re-pinned.
- 2026-11-28 — From M648: the fix is to register a volume-field dependency scene index and an HdDependencyForwardingSceneIndex for MoonRay's renderer name (as Storm/hdPrman/our Cycles fork do) and drop hdMoonray's MarkRprimDirty in field sync.
