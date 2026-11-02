# M278 - Translucent Selection Highlight

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Blocked on: M234 - Camera Projection Nodes merging (its translucent projection shader is what exposes the gap).

Storm (USD 26.08) never binds the selection buffer in its translucent (order-independent transparency) pass, so a selected translucent surface — every M234 - Camera Projection Nodes projected card, and any blended material — shows no selection highlight in the 3D viewport, although picking it works. Found while fixing M234's viewport checks (2026-11-01): a probe shader showed the selection buffer defined in the opaque pass and undefined in the translucent one, before and after selecting.

Acceptance sketch:
- Research pxr (HdxSelectionTask, HdSt OIT render pass, renderPassOitShader) for a supported way to get selection into the translucent pass before designing; no side channel through the stage (no "selected" primvar).
- Preferred direction: a Natron-side overlay — the viewport already renders a prim-ID pass for picking; tint the selected prims' pixels from it after the beauty pass.
- Selected translucent and opaque surfaces highlight the same way; a GL test selects a half-transparent projected card and checks the highlight.
