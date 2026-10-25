# M220 - SplineWarp Follow Vectors

Expose M219 - Curves Follow Vectors in SplineWarp. The Follow Vectors toolbar group appears on SplineWarp's viewer toolbar. Whatever is selected follows, as in Roto: source curves, destination curves, single-point curves and correspondence points alike, with no special pairing logic. M219 already does the work in the geometry base, so this milestone covers the SplineWarp-specific pieces: the toolbar hook-up, correspondence points as a followable item, and making sure following keeps the source/destination links and the warp field's invalidation intact.

Deps:
- M219 - Curves Follow Vectors: the engine, the toolbar group and the Python call.
- M148 - SplineWarp Viewer Tools: SplineWarp's interact and toolbar, the correspondence tool, and visibility in menus.

Scouted facts: none yet. Neither SplineWarp nor its context exists on `origin/main` at `3c8553ec`. Re-check M147 - SplineWarp Node Core's and M148 - SplineWarp Viewer Tools's merged files at promotion, especially `Engine/Nodes/Transform/SplineWarp/SplineWarpContext.{h,cpp}` and the correspondence-point storage.

## Scope

**In:**
- The Follow Vectors group on SplineWarp's toolbar.
- Following selected SplineWarp curves of either role, and selected correspondence points, which move per-point and are keyed at their own animation.
- `Roto.followVectors` working on a SplineWarp's curve context.

**Out:**
- Pairing logic: moving the partner curve, or keeping the warp offset → none (user: whatever is selected).
- A new motion model → none. M219's models apply.

**Edge cases:**
- **Following one end of a linked pair:** the link is kept, and only the selected curve's keys change.
- **A fitted model on a SplineWarp curve:** this keys the curve's transform, as in Roto. If M147 gives SplineWarp curves no transform, fits bake into point keys instead, and that is recorded as a decision at promotion.
- **The vector source is downstream of the SplineWarp,** for example a VectorGenerator on its output: refused by M219's cycle check.

**Assumptions:**
- Unconfirmed: SplineWarp curves are `Bezier` items in a `RotoCurveContext`, so M219's bake path writes them unchanged, and only the correspondence points need a new adapter.

## Phase 220.1: SplineWarp hook-up

- [ ] M220.P1.T1 — Toolbar group and correspondence points
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarp.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarpContext.{h,cpp}`, `Engine/VectorFollow.cpp`
  - approach:
    - Add M219's Follow Vectors group to SplineWarp's toolbar through the same builder.
    - Give `SplineWarpContext` a `followVectors` override that adds correspondence points as per-point items, then defers to the base.
    - Make sure the warp field's hash and invalidation run once per follow, not once per key.
  - verify: at the phase build, `ctest -R SplineWarpFollowVectors` (T2) passes.
  - size: M

- [ ] M220.P1.T2 — Test SplineWarp following
  - files: `Tests/SplineWarpFollowVectors_Test.cpp` (new)
  - approach:
    - On a translating fixture through VectorGenerator, check that:
      - a selected source curve follows while its linked destination stays;
      - a selected destination follows;
      - a correspondence point follows;
      - the links survive;
      - a render after the follow reflects the new keys;
      - one undo reverts everything.
  - verify: `ctest -R SplineWarpFollowVectors` passes.
  - size: M

**Verification gate:**
- The build and the full ctest pass, including `SplineWarpFollowVectors`.
- Xvfb GUI check, with screenshots shared before sign-off: follow a SplineWarp source curve and a correspondence point on a fixture plate, and scrub the warped output.
- An AppImage built with `package.sh` for UAT.

## Decisions

- 2026-10-25 — **SplineWarp follows whatever is selected (user),** with no pairing logic.
