# M652 - Cycles Shutter Dirtying Without Emulation

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Found by M648 - Cycles Live Volume Refresh's review (PR #412): the Cycles fork's render pass still calls `HdChangeTracker::MarkAllRprimsDirty` in `SetMotionShutter` (`src/hydra/render_pass.cpp` ~:372) and on subdivision setting changes (~:422). Under USD 26.08's terminal scene index the change tracker has emulation off, so each call logs "Calling method on HdChangeTracker that requires emulation" and may not dirty anything. The comment above :422 claiming Hydra accepts it is wrong. Both date from M554 - Scene Camera For Delegate Renders' pin.

Acceptance sketch:
- Shutter and subdivision changes reach the rprims through the scene index (a Cycles filtering scene index that dirties the affected locators, or the render settings / camera data sources Hydra already forwards), with no emulation-only calls left in the fork.
- An open-shutter volume or motion-blur test, and a subdivision setting change test, assert no "emulation" coding errors and that the image updates.
- Re-pinned in Natron.
