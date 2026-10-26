# M238 - EdgeExtend Options

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Follow-up the user asked for when approving M123 - Native Sharpen And Glow's UAT (2026-10-26): improve the native EdgeExtend's options. Today it has Source Premultiplied, Threshold (0.5), Size, Slices and Premultiply Output (M123 round 1).

Reference for the grill: Nuke 17's EdgeExtend (`.local/docs/nuke/html/content/reference_guide/filter_nodes/edgeextend.html`):
- a **Matte** input and a `matte` choice: None, Source Alpha, Source Inverted Alpha, Matte Luminance, Matte Inverted Luminance, Matte Alpha, Matte Inverted Alpha;
- **Erode** (negative dilates) applied to the matte region before extending;
- **Detail Amount** (default 1): how much of the original high-frequency detail and noise is recovered after the erode/dilate;
- **Output Edge Mask** to a chosen channel, so the edge region can be worked on separately;
- a fill algorithm independent of the fill region's size (ours is slice-count/size bound).

Ask the user which of these they want and whether anything else is missing before elaborating. Labels follow script names (`erode` → "Erode"); EdgeExtend is a filter, so no (un)premult-by knob.

Acceptance sketch:
- Each chosen option has CPU and GPU kernels with parity, native tests and goldens.
- The panel fits the 1920x1080 default layout; screenshots shared before sign-off.
