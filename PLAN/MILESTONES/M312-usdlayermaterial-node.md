# M312 - USDLayerMaterial Node

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A scene node that wraps chosen channels of the bound material as mix(original, paint layer, alpha), so TexturePaint layers overlay an existing look without baking it. By default it reads the layer→material-purpose mapping from stream metadata that TexturePaint writes. Backends: UsdPreviewSurface and USDProject3D's glslfx now, a MaterialX backend slot for later (user, 2026-11-02 /cat-grill).

Blocked on: M53 - Projected Texture Painting's TexturePaint core: its paint-layer outputs and the layer→purpose stream metadata it writes.

Acceptance sketch:
- Painting on a mesh with an existing material shows original and paint mixed per channel by the layer's alpha.
- Channels map to material inputs from the stream metadata by default and can be overridden per channel.
- Works for UsdPreviewSurface and USDProject3D glslfx materials; MaterialX is a declared, unimplemented backend slot.
