# M434 - Interacts On The Overlay Draw List

Move the native interacts' drawing off raw GL and onto M165 - Rich Viewer Overlays' `OverlayDrawList`. The geometry then sits as canonical-space points that a warp can map per vertex (M435 - Overlays Deform Through Warps), and handles, points and text stay screen-sized. The interacts ported are Roto/RotoPaint, Tracker, SplineWarp, and the host Transform/CornerPin/Position/Rectangle interacts. **Only drawing moves.** Each interact keeps its pen and key logic, which already receives canonical pen positions. The result must look the same as today. This is the E14 port M165 - Rich Viewer Overlays left out of its scope (its `## Scope` Out line), filed here because overlay deformation needs it.

Scouted 2026-11-14 on `origin/main`:
- `Engine/OverlayDrawList.h:80` offers points, lines, polylines, filled polygons, strokes, hit-testable handles and text. Sizes are in screen pixels, anchored at canonical points.
- `EffectInstance::buildOverlay_public` feeds `drawOverlayDrawList` (`Engine/EffectInstance.cpp:4101`).
- Raw-GL interacts:
  - `Engine/RotoPaintInteract.cpp` (1243 lines) and `Engine/RotoCurveInteract.cpp` (4872), reached from `RotoPaint::drawOverlay` (`Engine/RotoPaint.cpp:997`);
  - `Engine/TrackerNodeInteract.cpp` (1822);
  - `Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.cpp`;
  - the host interacts in `Gui/HostOverlay.cpp` (4169).

## Scope

**In:**
- Draw-list additions the interacts need, each drawn in screen space at canonical anchors or as canonical geometry:
  - dashed (stippled) polylines;
  - arrowheads at a polyline's end;
  - a per-primitive "screen-space shape at anchor" for crosshairs, rotation arcs, tangent ends and Tracker search/pattern boxes. Those are drawn about a canonical anchor and oriented by an optional canonical direction;
  - draw-order layers.
- Curves (Béziers, SplineWarp curves, feather) emitted as canonical polylines from the curve evaluation. Each primitive carries a "deformable" flag (geometry) versus "anchored" (screen glyph).
- Roto/RotoPaint, Tracker, SplineWarp and the four `HostOverlay` interacts draw only through the list.
- A per-interact Xvfb screenshot test against a captured before image.

**Out:**
- Mapping anything through warps: M435 - Overlays Deform Through Warps.
- OFX plug-in overlays (`OfxOverlayInteract`): they stay raw GL, and M435 - Overlays Deform Through Warps gives them a texture-warp fallback.
- Changing any interact's behaviour, hit-testing or look. A visible difference is a bug, not a bonus.
- 3D viewer widgets (`Gui/SceneGizmo*`, `SceneTransformWidget*`).

**Edge cases:**
- RotoPaint brush cursor and stroke preview: emitted as strokes and anchored circles, keeping today's size in canonical units.
- Very dense shapes (hundreds of Béziers, M224 - Fluid Roto And RotoPaint Interaction scale): emission must not regress the GUI bench. Build the list once per redraw, and reuse curve tessellation already cached for rendering where it exists.
- Selection rectangles and text labels: anchored (text) or viewport geometry (the rubber band, which is drawn in viewport space and never deformed).

**Assumptions:**
- Unconfirmed: every raw-GL call in these interacts has a draw-list equivalent once the additions above land. P1.T1's audit is the check, and anything left over is reported before porting.

## Phase 434.1: Draw-list additions

- [ ] M434.P1.T1 — Audit the interacts' GL calls
  - files: read-only `Engine/RotoPaintInteract.cpp`, `Engine/RotoCurveInteract.cpp`, `Engine/TrackerNodeInteract.cpp`, `Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.cpp`, `Gui/HostOverlay.cpp`; write `docs/dev/overlay-draw-list-port.md`
  - approach: list each distinct drawing construct (GL primitive, stipple, line width, blend, glyph, text, overlay-viewport call) with file:line. Map each one to an existing draw-list primitive or to one of the planned additions. Flag anything unmapped.
  - verify: the doc covers every `gl*` and `OverlaySupport` drawing call in the five files (`grep -c` the call counts in the report).
  - size: M

- [ ] M434.P1.T2 — Draw-list primitives for interacts
  - files: `Engine/OverlayDrawList.{h,cpp}`, the viewport drawer (`drawOverlayDrawList` implementation in `Gui/ViewerGL*.cpp`), `Tests/OverlayDrawList_Test.cpp`
  - approach:
    - Add a dash pattern on polylines/lines, in screen pixels.
    - Add an arrowhead option.
    - Add `addAnchoredShape(anchor, direction?, shape)` with shapes cross, circle, arc(start, span, radius px), box(w, h px) and the rest P1.T1 needs.
    - Add `layer` ordering and a `deformable` flag (true for geometry primitives, false for handles, points, text and anchored shapes).
    - The drawer renders the new types.
  - verify: gtest on list building (ranges, flags, ordering); an Xvfb screenshot of a test node drawing one of each new primitive, shared with the user.
  - size: M

## Phase 434.2: Ports

- [ ] M434.P2.T1 — Before-image capture harness
  - files: `Tests/gui/m434_interact_parity.py`, reference crops under `Tests/gui/references/m434/` (≤256 px crops, compressed)
  - approach:
    - One scene per interact: a Roto with Béziers, feather and a selected point with tangents; a RotoPaint stroke plus cursor; a Tracker with two tracks; a SplineWarp pair; Transform, CornerPin, Position and Rectangle.
    - Capture viewer crops on current main before any port, and commit them as references.
    - The script compares within a small per-pixel tolerance, to allow line antialiasing differences.
  - verify: the script passes on unported main.
  - size: M

- [ ] M434.P2.T2 — Port Roto/RotoPaint drawing
  - files: `Engine/RotoCurveInteract.cpp`, `Engine/RotoPaintInteract.cpp`, `Engine/RotoPaint.{h,cpp}` (override `buildOverlay`/`hasOverlayDrawList`, drop the GL path)
  - approach: emit every construct per P1.T1's map. Béziers and feather become canonical polylines from the curve's evaluated points, at the density the render uses or adaptive to zoom. Points, tangent ends and the selection box become handles or anchored shapes. Pen code is untouched.
  - verify: `m434_interact_parity.py` Roto and RotoPaint scenes pass; the GUI bench is run and compared (Roto edits on many shapes) per the board's GUI bench rule.
  - size: L

- [ ] M434.P2.T3 — Port Tracker drawing
  - files: `Engine/TrackerNodeInteract.cpp`, `Engine/TrackerNode.cpp`
  - approach: as P2.T2. The pattern and search boxes are canonical polylines, since they deform with the image. Centre crosses and labels are anchored.
  - verify: the Tracker parity scene passes.
  - size: M

- [ ] M434.P2.T4 — Port SplineWarp drawing
  - files: `Engine/Nodes/Transform/SplineWarp/SplineWarpInteract.cpp`, `SplineWarp.{h,cpp}`
  - approach: as P2.T2. Source and destination curves and correspondence lines are deformable, and points are handles.
  - verify: the SplineWarp parity scene passes; the existing SplineWarp viewer-tools GUI tests pass.
  - size: M

- [ ] M434.P2.T5 — Port host Transform/CornerPin/Position/Rectangle interacts
  - files: `Gui/HostOverlay.cpp`, `Gui/HostOverlay.h`, and the engine side that feeds it (`Engine/HostOverlaySupport.h`)
  - approach: emit through a draw list per host interact. The Transform's centre, rotation bar and scale circle are anchored shapes about canonical anchors, with directions from the transform. CornerPin edges are deformable polylines and its corners are handles.
  - verify: the four host-interact parity scenes pass; existing Transform/CornerPin GUI tests pass.
  - size: L

**Verification gate:** `OverlayDrawList_Test`, `m434_interact_parity.py` and every existing Roto, Tracker, SplineWarp and Transform GUI test pass on a remote release build with `--gui`. The GUI bench (`tools/bench/gui/run.sh --xvfb` + `compare.py`) shows no regression, and the result is recorded in `## Decisions`. Screenshots of each ported interact go to the user before sign-off. The UAT is collected at F25 - Overlay Flow's landing.
