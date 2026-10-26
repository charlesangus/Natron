# M224 - Fluid Roto And RotoPaint Interaction

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Found during M146 - Roto Hosting For Any Node's UAT (user, 2026-10-26), pre-existing: RotoPaint interaction in the viewer is poor. Strokes don't draw cleanly and instantly under the pen; they appear after a delay. Roto and RotoPaint interactivity is central to the user experience and must be completely fluid.

Investigate before designing: profile a live stroke end to end (input event → stroke sample → overlay draw → incremental render → texture upload → viewer paint), on the hardware range (a modest laptop GPU as well as a workstation), with brush sizes from small to large, at 2K and 4K, with and without a heavy upstream graph. Compare with how Nuke, Krita and Mari keep strokes under the pen (immediate overlay or preview stroke drawn on the GPU, render catching up behind). Check the existing paint-stroke path (the `setDuringPaintStrokeCreationThreadLocal` tiled path, the stroke render queue, cache behaviour during a stroke) for avoidable work and waits.

Acceptance sketch:
- A stroke follows the pen with no visible lag: the pen-to-pixel latency is measured and held to a budget the user signs off (one display frame for the preview), with the final render converging behind it.
- Roto shape editing (dragging points, tangents, feather) is equally fluid on large shapes and many shapes.
- No regression in the rendered result: the finished stroke is bit-identical to today's.
- A benchmark records stroke latency and frame times so regressions show up.
