# M294 - ZDefocus Engine Revisit

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Revisit ZDefocus's layered gather using whatever M291 - Defocus Reference Harness shows beats it. The candidates are Niik-l's quality ideas (`niikl/RB-2.6:Engine/Dev/PostFX/DefocusCore.cpp`):
- splitting edge pixels into front and back samples ("Unmix", estimated from clean neighbours);
- forced slice boundaries at depth edges;
- a distance-transform fill behind the foreground, weighted by alpha;
- coverage renormalisation;
- per-sample row-span scatter in place of the gather;
- P/N-aided occlusion edges, if the harness shows they help.

Edge Split, Surface Edges and the `defocusError` plane join ZDefocus's `output` choice for whichever stages are adopted. The engine stays **fully GPU-resident** (user, 2026-11-01): slicing, fill and scatter (or gather) all run on the GPU, with no CPU round-trip.

Deps: M291 - Defocus Reference Harness (results), M292 - Traced Lens Bokeh and M293 - ZDefocus Debug Outputs (the revisited engine must keep traced lenses, image stages and debug modes working).

Blocked on: M291 - Defocus Reference Harness's results and the user's choice of direction from its report.

Acceptance sketch:
- The harness's edge-band RMSE and halo/hole energy improve on today's ZDefocus for every fixture scene, at the chosen quality settings.
- Every existing ZDefocus control, M133's aberrations, M292's traced lenses and stages, and M293's debug modes keep working. Any golden changes are explained.
- The GPU path is resident end to end and passes parity against the CPU path. Performance is measured on modest and many-core hosts and stays within budget at 4K with a 256 px maximum blur.
- The harness numbers become a ctest regression bound.

## Scope

**Out:**
- Deep input: M295 - Native DeepDefocus.
- A separate LensDefocus node: not wanted (user, 2026-11-01).
- Tilt focus, World Point, camera focus link, extra depth conventions and CleanBg: not requested (user, 2026-11-01); revisit only if the harness shows CleanBg is needed for fill quality.

## Decisions

- 2026-11-01 — **GPU residency (user, /cat-plan):** asked whether the GPU should do only the scatter, as Niik-l's does, the user chose fully GPU-resident.
