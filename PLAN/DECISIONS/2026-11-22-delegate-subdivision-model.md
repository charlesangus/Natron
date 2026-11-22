# 2026-11-22 — Delegate subdivision model (user)

Cycles and MoonRay render USD subdivision surfaces the way Karma and Gaffer do: a mesh's
`subdivisionScheme` decides whether it is a subdivision surface, and screen-space dicing
settings (global, with per-prim primvar overrides, and a dicing camera) decide its density.
Each renderer has a `Subdivision Surfaces` toggle, off by default, so USD's `catmullClark`
fallback doesn't subdivide meshes that never asked for it. This keeps M284 - Utility Renderer
Subdivision's "level 0 unless specifically changed" intent. Hydra's display `refineLevel` no
longer gates or sets subdivision in these renderers; it remains a viewport-complexity concept.
There is no per-mesh opt-in primvar over the toggle. Loop meshes render as Catmull-Clark with
a warning. The Utility renderer moves to the same model in M289 - Utility Adaptive
Subdivision. Implemented in M494 - Delegate Subdivision.
