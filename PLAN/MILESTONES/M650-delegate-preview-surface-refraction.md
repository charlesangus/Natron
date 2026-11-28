# M650 - Delegate Preview Surface Refraction

Found in M256 - Delegate AOV Selection's UAT (user, 2026-11-28): a UsdPreviewSurface with refraction
(`opacity` < 1 with `ior`, i.e. a glass-like material) does not render as refractive in Cycles. MoonRay
is unchecked; the user's current host can't run it, so check it on a GPU VM.

## Scope

**In:**
- Reproduce in Cycles and MoonRay with a minimal UsdPreviewSurface glass scene (opacity, ior, opacityThreshold unset), compared with Storm and Natron Utility (M282 - Utility Reflection And Refraction).
- Find where the UsdPreviewSurface → delegate material translation drops or misreads refraction: hdCycles' UsdPreviewSurface network conversion (`src/hydra/material.cpp` in the Cycles fork) and hdMoonray's (`lib/hydramoonray` material translation in the fork).
- Fix it in the fork(s) on branches off the current pins, re-pin in `tools/ci/local/Dockerfile`, and add a render test per delegate.

**Out:**
- MaterialX / native Cycles or MoonRay shading networks.

**Edge cases:**
- `opacity` < 1 with `ior` = 1 should be plain transparency, not refraction; with `opacityMode` presence/transparent as UsdPreviewSurface 2.6 defines.
- `opacityThreshold` > 0 is cutout, not refraction.

## Phase 650.1: Reproduce and fix

- [ ] M650.P1.T1 — Reproduce on Cycles and MoonRay
  - files: `Tests/Scene/` (new fixture and test), a GPU VM run for MoonRay if the CI MoonRay lacks a needed feature
  - approach: a glass sphere in front of a textured card; assert the card behind is refracted (offset/inverted), not just faded
  - size: M
- [ ] M650.P1.T2 — Fix the delegate translation(s) and re-pin
  - files: the Cycles fork `src/hydra/material.cpp`, the openmoonray/hdMoonray forks, `tools/ci/local/Dockerfile`
  - size: M

**Verification gate:** the refraction tests pass in Cycles and MoonRay on a remote gate; the user confirms in UAT.
