# M153 - Distortion Concatenation

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

An engine-wide mechanism for concatenating non-matrix warps, in both directions, so a chain like Transform → SplineWarp → Transform (or IDistort, LensDistortion, CornerPin) resamples its source once.
- A node that can distort exposes an **inverse map**: output pixel → input coordinate, with an optional Jacobian, over a window.
- The engine composes the maps of adjacent concatenating nodes the way `tryConcatenateTransforms` (`Engine/EffectInstance.cpp:2208-2315`) composes 3x3 matrices today.
- The consumer at the end of the chain samples the original source through the composed map, using M113 - Native Warp Nodes's coordinate sampler.
- RoI propagation goes through the composed map.

This is the conceptual equivalent of OFX standard change 243's `getInverseDistortion`, which the vendored host lib defines but the engine never consumes (`libs/OpenFX/HostSupport/src/ofxhClip.cpp:81,833`, `ofxhImageEffect.cpp:159,1706`, `libs/OpenFX/include/ofxNatron.h:1032+`).

SplineWarp is the first client in both directions: a downstream Transform folds into SplineWarp's map, and SplineWarp folds into a downstream warp. Converting M113 - Native Warp Nodes's IDistort, STMap and CornerPin, and M132 - Native Lens Distortion, follows.

Blocked on: M147 - SplineWarp Node Core and M113 - Native Warp Nodes landing. The map interface's shape (a field versus a per-pixel callback, Jacobian handling, how filtering composes, GPU representation) has to be designed against the coordinate field and sampler those two actually build, not the planned ones.

Acceptance sketch:
- Transform → SplineWarp → Transform renders with one resample, and matches a single-resample reference within 1e-4.
- SplineWarp → IDistort concatenates the same way, and the RoI requested from the source is the composed map's footprint.
- A chain that can't concatenate (a non-distorting node in between, or motion blur that can't compose) falls back to per-node resampling with identical results to today.
- No regression in the existing matrix concatenation tests or the GUI and render benches.

## Decisions

- 2026-10-20 — **Separate milestone, after SplineWarp v1 (user):** engine architecture is kept out of node work. SplineWarp v1 absorbs upstream transforms only (M147 - SplineWarp Node Core).
