# M152 - Curve Track Links And Roto Copy

Two curve-host features that Roto and SplineWarp share, both built on M146 - Roto Hosting For Any Node's geometry base:
1. **Track links:** a control point can follow a Tracker track, offset from the track's centre as it was at the link frame. The whole point moves, tangents and feather included.
2. **Cross-node copy and paste:** curves copy between any two curve hosts (Roto to SplineWarp, SplineWarp to SplineWarp, and so on) through the system clipboard.

Deps: M148 - SplineWarp Viewer Tools (the SplineWarp interact and panel hook that the UI goes into).

Scouted facts (2026-10-20, `main` at `8fdb5741f`):
- There is no track linking for roto control points today. Grepping `BezierCP`, `Bezier`, `RotoPaint*` and `RotoPanel` for track or slave hooks finds none.
- A track's centre is `TrackMarker::getCenterKnob()` (`Engine/TrackMarker.h:144`, knob `kTrackerParamCenter` "centerPoint", `:63`). Track markers belong to a `TrackerContext` on a Tracker node (`Engine/TrackerContext.h`).
- `BezierCP` holds X/Y and left/right tangent `Curve`s (`Engine/BezierCP.h:76-81`). `getPositionAtTime(useGuiCurves,time,view,&x,&y)` is the read path.
- `RotoPanel`'s clipboard is a member list of tree items (`Gui/RotoPanel.cpp:388`) with `PasteItemUndoCommand` (`:206-239`, copy-to-layer and copy-to-item modes). Copy and paste is within one panel only.
- Item serialization: `Bezier::toSerialization/fromSerialization` (`Engine/Bezier.h:618`), `Serialization/RotoSerialization.h`.

## Scope

**In:**
- **`BezierCP` track link:** `{tracker node script-name, track script-name, offset}`. The evaluated position is `track centre(t) + offset`, and the tangents and the feather point translate with it.
  - It is serialized, and the curve host's content hash includes the track centre's curve hash.
  - The curve host re-renders when the track changes.
- **UI** in `RotoCurveInteract`, so Roto gets it too: CP context-menu actions Link to Track… (choose from every track in the project), Unlink, and Unlink and Bake (keyframes at every frame of the track's keyed range). Linked CPs are drawn distinctly. Undo for all of them.
- **Python:** `BezierCurve.linkPointToTrack(index, track)` and `unlinkPoint(index, bake)`.
- **Cross-node copy and paste:**
  - Panel copy writes the serialized items to the system clipboard under a Natron mime type. Paste reads it into any curve host.
  - Pasting a SplineWarp source and destination into a SplineWarp keeps their link, when both ends are in the clipboard.
  - Pasting into Roto drops the warp data. Pasting from Roto into SplineWarp brings the curves in unlinked.

**Out:**
- Linking whole curves to a track's transform, and stabilisation → none.
- Pasting strokes into a geometry host → none (refused: strokes are paint-only).

**Edge cases:**
- The tracker node or track is deleted or renamed: the CP keeps its last evaluated position as static, and a node warning names the missing track. A rename is followed if the track is still resolvable by object, through the existing rename notifications.
- A track with no keyframes at the current time: the track's own interpolation and extrapolation apply.
- Linking a CP that is already animated: the offset is taken at the link frame, and existing keyframes are kept but ignored while linked. Unlink restores them, and Unlink and Bake replaces them.
- Paste of a link whose partner isn't in the clipboard: it is pasted as an unlinked curve.
- Paste between projects (another app instance): track links survive only if the names resolve. Otherwise they become static, as for a missing track.

**Assumptions:**
- Tracker centre knobs can be watched for changes from another node's curve context through the existing knob-listener mechanism, without touching the tracker's render.

## Phase 152.1: Track links

- [ ] M152.P1.T1 — Track link on `BezierCP`: model, evaluation and serialization
  - files: `Engine/BezierCP.{h,cpp}`, `Engine/Bezier.cpp`, `Serialization/RotoSerialization.h`, `Tests/CurveTrackLink_Test.cpp` (new)
  - approach:
    - Add an optional link. `getPositionAtTime` returns `track centre(t) + offset` when it is linked, and the tangents and feather translate by the same delta.
    - Linking stores the offset at the link time.
    - Serialize the link with the CP. Resolve the names on load, after every node exists. A failed resolve falls back to static, with a warning.
  - verify: the test covers a CP following a keyed track (1e-9), the offset being preserved, the tangent delta, serialization round-trip, and the missing-track fallback.
  - size: L

- [ ] M152.P1.T2 — Track-driven invalidation
  - files: `Engine/Bezier.cpp`, `Engine/RotoCurveContext.cpp`, `Engine/TrackMarker.cpp`, `Tests/CurveTrackLink_Test.cpp`
  - approach: fold each linked track's centre-curve hash into the curve host's content hash, and listen to the centre knob so an edit re-evaluates the host. Remove the listener on unlink and on delete.
  - verify: the test moves a track keyframe and checks that the SplineWarp's and the Roto's hashes change and that a render reflects it.
  - size: M

- [ ] M152.P1.T3 — Link to Track UI and Python
  - files: `Engine/RotoCurveInteract.cpp`, `Engine/RotoUndoCommand.{h,cpp}`, `Engine/PyRoto.{h,cpp}`, `Engine/typesystem_engine.xml`
  - approach:
    - Add CP context-menu actions: Link to Track… (a chooser listing `tracker / track` across the project), Unlink, and Unlink and Bake. Each is an undo command.
    - Draw linked CPs with a distinct marker.
    - Add the Python methods.
  - verify: a Python test (`Tests/CurveTrackLink_Test.cpp` via the interpreter) links, bakes and unlinks. The gate's Xvfb pass covers the menu.
  - size: M

## Phase 152.2: Cross-node copy and paste

- [ ] M152.P2.T1 — Clipboard copy and paste between curve hosts
  - files: `Gui/RotoPanel.{h,cpp}`, `Engine/RotoCurveContext.{h,cpp}`, `Engine/Nodes/Transform/SplineWarp/SplineWarpContext.cpp`, `Tests/CurveClipboard_Test.cpp` (new)
  - approach:
    - Add `RotoCurveContext::serializeItems(items)` and `pasteItems(blob, layer)`. They are virtual, so `SplineWarpContext` adds and restores the warp data.
    - The panel's copy puts the blob on `QClipboard` with an `application/x-natron-curves` mime type. Paste uses the existing `PasteItemUndoCommand` in copy-to-layer mode, fed from the blob.
    - Refuse strokes into geometry hosts. Drop warp data into non-SplineWarp hosts, and drop links whose partner is missing.
  - verify: the engine-level test (serialize, then paste) covers Roto → SplineWarp (unlinked), a SplineWarp pair → SplineWarp (linked), SplineWarp → Roto (no warp data), and a one-ended link → unlinked. The gate's Xvfb pass covers the panel round-trip.
  - size: L

**Verification gate:**
- The build and the full ctest pass, including `CurveTrackLink_Test` and `CurveClipboard_Test`.
- Xvfb GUI check, with screenshots shared before sign-off:
  - Track a fixture, link a SplineWarp destination CP and a Roto CP to the track, and scrub to see both follow.
  - Unlink and bake.
  - Copy a Roto shape into a SplineWarp, and a linked pair between two SplineWarps.
- Run the GUI bench if the diff touches the bench rule's paths.
- An AppImage built with `package.sh` for UAT.

## Decisions

- 2026-10-28: Implemented (build pending). Track-link offsets are in the curve's own coordinates; only the track centre is used. Moving a linked point keys its stored curves; Unlink and Bake keys the whole curve. Menu knobs linkToTrack, unlinkTrack, unlinkTrackAndBake (unlink only when linked). Undo snapshots whole curves. Missing-track warning: "Control points follow tracks that cannot be found and stay where they last were:". Track chooser via a new AppInstance::chooseItemDialog (QInputDialog; −1 headless). Clipboard: roto YAML with blobs inlined as application/x-natron-curves; strokes dropped and counted outside paint hosts; SplineWarp boundaries/links carried only when all named curves were copied; clashing names get _copy, _copy2…; pasted items go on top in clipboard order. Known gaps: link/bake write curves on the main thread while a render may read them; the missing-track warning can flash during load and clearing it clears other persistent messages; nested layers re-added only at top level on redo (pre-existing).
- 2026-10-20 — **Track links and Roto copy are in scope (user).**
- 2026-10-20 — **Both live in the geometry base (planner):** Roto gets track-linked points and cross-node paste too, at no extra cost.
