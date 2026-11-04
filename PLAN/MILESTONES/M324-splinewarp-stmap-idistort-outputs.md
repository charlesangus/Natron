# M324 - SplineWarp STMap And IDistort Outputs

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Let SplineWarp output its warp as data ready to drive an STMap and an IDistort: normalised source UVs for STMap and per-pixel displacement (forward/backward vectors) for IDistort, as extra layers, so the warp can be baked, reused on other plates, or applied downstream (user, 2026-11-04, SplineWarp UAT).

Blocked on: the SplineWarp stack (M147–M152, M274) landing with its Round 2 UAT fixes (final solver and render path), and checking Natron's STMap/IDistort conventions (UV origin, pixel centre, displacement direction and units) so the outputs plug in without conversion.

Acceptance sketch:
- SplineWarp → STMap on the same source reproduces SplineWarp's output within filtering tolerance.
- SplineWarp's displacement layer → IDistort reproduces it likewise.
- Layers named and documented; motion blur and Amount respected.
