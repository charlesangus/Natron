# M274 - SplineWarp Remaining Controls

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

M147 - SplineWarp Node Core shipped the core node (amount, invert, TPS solver, smoothness, curve/field resolution, filter, clamp, black outside, BBox, stage knobs). The rest of Nuke 17's SplineWarp controls are planned nowhere else; a port means every control, so they land here. Reference: `.local/docs/nuke/html/content/reference_guide/transform_nodes/splinewarp.html`.

Acceptance sketch:
- Params → knob audit against the Nuke reference: every SplineWarp control either exists with Nuke's default, or is recorded here as deliberately dropped with the user's sign-off.
- Covers at least: morph (A/B blend between source and destination images), cookie-cutter / root warp and boundary behaviour, the Transform tab (per-curve and global transform of source/destination), the curve, boundary and preview resolution controls, Lanczos/Sinc filters (shared resampler, M113), output mask, crop to format.
- CPU first; GPU follows M150 - SplineWarp GPU Kernel's kernel. Native tests, goldens (≤256 px half-float crops), GUI check with screenshots.

Deps: M147, M148 (viewer tools own the interactive parts of the Transform tab).
