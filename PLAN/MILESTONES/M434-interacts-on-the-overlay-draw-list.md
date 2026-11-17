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
- A per-node draw-list cache. A node's list is rebuilt only when its content, selection, hover, time or zoom bucket changes, and the GPU buffer is kept while the list is unchanged.
- An overlay draw-cost benchmark, recorded on main before any port. The ports must stay within 10% of it.

**Out:**
- Mapping anything through warps: M435 - Overlays Deform Through Warps.
- OFX plug-in overlays (`OfxOverlayInteract`): they stay raw GL, and M435 - Overlays Deform Through Warps gives them a texture-warp fallback.
- Changing any interact's behaviour, hit-testing or look. A visible difference is a bug, not a bonus.
- 3D viewer widgets (`Gui/SceneGizmo*`, `SceneTransformWidget*`).

**Edge cases:**
- RotoPaint brush cursor and stroke preview: emitted as strokes and anchored circles, keeping today's size in canonical units.
- Very dense shapes (hundreds of Béziers, M224 - Fluid Roto And RotoPaint Interaction scale): emission must not regress the GUI bench. Build the list once per redraw, and reuse curve tessellation already cached for rendering where it exists.
- Selection rectangles and text labels: anchored (text) or viewport geometry (the rubber band, which is drawn in viewport space and never deformed).

- Hover and pen-motion redraws that change only the hovered or dragged item: the cache invalidates by content version, so a drag rebuilds only the dragged node's list, never every open overlay's.

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

- [ ] M434.P1.T3 — Overlay draw-cost benchmark, baseline on main
  - files: `tools/bench/gui/roto_bench.py` (new ops), `tools/bench/gui/README.md`, `tools/bench/gui/baselines/` (new baseline record)
  - approach:
    - Add an `overlay_draw` op: the main-thread time of `ViewerTab::drawOverlays` per redraw, read through a timing probe (an env-gated `NATRON_OVERLAY_DRAW_LOG` like the existing latency probe). It runs during a hover pass and during a point drag.
    - Shape counts include 500 as well as the defaults 1, 30 and 100, each shape with feather and one shape selected so tangents draw.
    - Add a Tracker topology with 50 tracks, measured the same way.
    - Record median and p95.
    - Run it on unported main under `--xvfb` and commit the baseline.
  - verify: `tools/bench/gui/run.sh --xvfb --driver roto` produces `overlay_draw` records for every shape count and the Tracker topology, and `compare.py` reads them.
  - size: M

- [ ] M434.P1.T4 — Draw-list cache and persistent GPU buffer
  - files: `Engine/EffectInstance.{h,cpp}` (`buildOverlay_public`, a list version), `Engine/OverlayDrawList.{h,cpp}`, `Gui/NodeOverlayRenderer.{h,cpp}`, `Tests/OverlayDrawList_Test.cpp`
  - approach:
    - Nodes declare an overlay content version: a counter bumped on any change that alters what they draw, plus time, view and a zoom bucket.
    - `buildOverlay_public` returns the cached list when the key is unchanged.
    - `NodeOverlayRenderer` keeps one renderer per node and skips triangle conversion and the buffer upload when the list version is unchanged.
    - Hover state is drawn as a small separate list, so hovering doesn't invalidate the main one.
  - verify: gtest — building twice with an unchanged key returns the same list without calling `buildOverlay`, and a version bump rebuilds; an Xvfb run of the M165 test node shows a second redraw doing no upload (a counter exposed for tests).
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

**Verification gate:** `overlay_draw` p95 for every shape count and the Tracker topology is within 10% of the P1.T3 baseline, or better (`compare.py`), and the result is recorded in `## Decisions`. `OverlayDrawList_Test`, `m434_interact_parity.py` and every existing Roto, Tracker, SplineWarp and Transform GUI test pass on a remote release build with `--gui`. The GUI bench (`tools/bench/gui/run.sh --xvfb` + `compare.py`) shows no regression, and the result is recorded in `## Decisions`. Screenshots of each ported interact go to the user before sign-off. The UAT is collected at F25 - Overlay Flow's landing.

## Decisions

- 2026-11-14 — The ports must keep overlay draw cost within 10% of today's raw-GL drawing, measured by a dedicated benchmark recorded before any port, and the draw list is cached between redraws (user): raw GL is cheap to issue, so the draw list's per-redraw rebuild and upload must not cost more than it saves.
- 2026-11-17 — P1.T1-P1.T4 and P2.T1 code in c01d83499 (unbuilt). Port map at docs/dev/overlay-draw-list-port.md. The list cache keys on content version, time, view, mipmap level and a zoom step of an eighth of an octave; hover gets its own list. The bench baseline is recorded from this branch before any port, since main has no probe. Next: record the bench baseline and the parity references on this build, then P2.T2-P2.T5.
- 2026-11-17 — **RampInteract is ported too (user):** it joins the HostOverlay ports in P2.T5.
- 2026-11-17 — **RotoPaint's stroke preview (GL_MAX against destination alpha), Tracker thumbnails and the magnifier stay on direct GL (user):** they draw beside the draw list, and M435 - Overlays Deform Through Warps gives them the same texture-warp fallback as OFX overlays.
