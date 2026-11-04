# M335 - Viewer Render Draw States

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Viewer renders through Cycles or MoonRay follow the viewer's draw states, so you can look-dev one
asset without paying for the whole scene, with a Render Everything toggle that ignores them.
Design: `PLAN/DESIGN/2026-11-04-scene-graph-design.md`.

Blocked on: M327 - Heavy Scene Spike's mechanism decision and M254 - Progressive Viewer Render's engine shape, which together determine how a delegate engine receives draw states.

Acceptance sketch:
- With one asset Full + Load in the payload city, a Cycles viewer render shows only that asset and opens no other payload.
- BBox branches don't appear in the render; their boxes stay drawn as overlay.
- Render Everything loads the whole scene and matches what USDRender produces.
- USDRender and USDWrite are unaffected either way.

## Scope

**Out:**
- USDRender and USDWrite: they never follow draw states (user).

**Edge cases:**
- Turning Render Everything on loads payloads, the one case of a viewer toggle doing so; the toggle's tooltip says it will.
