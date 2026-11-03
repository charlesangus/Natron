# M314 - Paint-Through Inputs

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Extra 2D inputs on TexturePaint. Each is Screen-aligned and movable in the 3D view, or projected through a scene camera, animated cameras included. The Reveal/Paint Through brush paints them onto the surface (user, 2026-11-02 /cat-grill).

Blocked on: M53 - Projected Texture Painting's TexturePaint core (brush and buffer model) and M234 - Camera Projection Nodes' camera projection.

Acceptance sketch:
- A TexturePaint node exposes extra 2D inputs, each with a Screen/Camera mode.
- In Screen mode the input can be moved, scaled and rotated in the 3D view; in Camera mode it projects through the chosen scene camera on every frame.
- The Reveal/Paint Through brush paints the input's pixels into the current layer.
