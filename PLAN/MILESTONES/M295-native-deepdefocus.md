# M295 - Native DeepDefocus

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A separate `DeepDefocus` node (user, 2026-11-01), as in Niik-l's fork (`niikl/RB-2.6:Engine/Dev/Deep/DeepDefocus.cpp`, `DefocusCore`'s `DeepView`). It defocuses every deep sample at its own CoC, splits volume segments by depth, and outputs a flat image. Unlike Niik-l's side-channel `getDeepImageFromEffect`, it fetches deep data through `renderDeepRoI` (E8 - Deep Compositing, `Engine/EffectInstance.h`), and it works at every render scale (theirs is limited to scale 1). It shares ZDefocus's camera, lens (M292 - Traced Lens Bokeh) and bokeh controls, with the depth-channel and depth-convention knobs hidden, since deep samples carry Z.

Deps: M294 - ZDefocus Engine Revisit.

Blocked on: M294 - ZDefocus Engine Revisit, whose chosen engine (scatter or gather, GPU-resident) DeepDefocus must reuse.

Acceptance sketch:
- A deep EXR or DeepFromImage input defocuses to a flat RGBA. A deep scene with correct holdouts shows no edge halos, compared through M291 - Defocus Reference Harness's renderer extended with a deep pinhole output.
- Every ZDefocus bokeh and lens control behaves the same on DeepDefocus.
- GPU-resident, with parity tests, docs and a GUI check.

## Scope

**Out:**
- Deep output (defocused deep samples): no milestone.
- Deep input on ZDefocus itself: not wanted (user chose a separate node).
