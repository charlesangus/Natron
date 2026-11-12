# M363 - Geometry Display Extras

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

The SOP display features left out of F15 - Native Geometry (user, M54 - SOPs-Style Geometry
grill, 2026-11-08): template and footprint display of other geometry nodes alongside the
displayed one, and attribute visualisers (colour ramps, markers, vectors and text from chosen
attributes). Drawn as guide prims in the viewer-private session layer, like
M376 - Geometry Display's core toggles, with no second GL pass.

Blocked on: M358 - Geometry Bridges And Display and M376 - Geometry Display landing the geometry display and toggle
mechanism, which these extend.

Acceptance sketch:
- Flagging a node as template shows its geometry in wireframe behind the displayed node's.
- An attribute visualiser colours points by a float attribute with a ramp, and draws a vector attribute as lines.
- Storm renders USDMaterial's "Multiply By Primvar" (deferred from M376.P2.T5, user 2026-11-12): M376 authors it only in the `cycles` render context; Storm needs a MaterialX duplicate on `outputs:mtlx:surface` (`ND_UsdPreviewSurface_surfaceshader`, `ND_geompropvalue_color3`, `ND_multiply_color3`, `ND_image_color3`) covering every input and texture.
