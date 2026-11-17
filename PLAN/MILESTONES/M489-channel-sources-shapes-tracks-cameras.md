# M489 - Channel Sources From Shapes, Tracks And Cameras

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Channel sources and targets beyond knobs:
- **Roto and SplineWarp shapes:** control points, feather points, translate and `extraMatrix` curves, as channels in and as export targets out. M453 - Follow Animation Layer needs these to drive shapes.
- **Tracks:** tracker tracks as channels, and the reverse.
- **Geometry:** native geometry point positions or attributes per frame, sampled by point number or group, and channels back onto points.
- **Cameras:** a USD or native camera's transform and lens channels, plus a Project node that turns 3D points (channels or geometry) into 2D screen-space channels through a camera, for pinning 2D work to 3D.

Blocked on: M486 - Channel Network Core and M487 - Channel Export And chop(), for the export target interface.

Acceptance sketch:
- Exporting channels onto a Bezier's control points animates the shape. Turning the export off restores its keys.
- Projecting a 3D point through a tracked camera gives screen positions that match the viewer's projection within a pixel.
- A track round-trips through channels unchanged.

## Scope

**Out:**
- Image and audio sources: M490 - Channel Sources From Images And Audio.
