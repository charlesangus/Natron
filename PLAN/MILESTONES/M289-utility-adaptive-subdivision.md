# M289 - Utility Adaptive Subdivision

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Screen-space adaptive dicing for Natron Utility's subdivision surfaces, as Karma and
RenderMan do: refine each patch to a target edge length in pixels (with an off-screen and
motion-aware rate), instead of M284 - Utility Renderer Subdivision's uniform level. It is the
natural base for M287 - Utility Renderer Displacement.

It also moves Utility to the subdivision model Cycles and MoonRay get in M494 - Delegate
Subdivision (user, 2026-11-22; `PLAN/DECISIONS/2026-11-22-delegate-subdivision-model.md`): a
`Subdivision Surfaces` toggle, off by default, after which every mesh whose scheme is not
`none` is a subdivision surface diced to the dicing settings. The `Subdivision Level` setting,
the `natron:subdivLevel` primvar and the viewer `refineLevel` precedence go (no backward
compatibility); Loop stays Loop, since Utility refines it natively.

Blocked on: M284 - Utility Renderer Subdivision being done — its stencil cache and refined
mesh path are what this replaces or extends, and the choice between OpenSubdiv feature-adaptive
patches and a custom dicer depends on how that code turns out.

Acceptance sketch:
- A `Subdivision Surfaces` toggle (default off) and a `Dicing Rate` setting (pixels per edge) replace the level, setting and primvar alike, with the same knob names as Cycles' in M494 - Delegate Subdivision.
- Close geometry is finer and distant geometry coarser, with no cracks between patches.
- Render time scales with screen coverage, not cage size × level.
