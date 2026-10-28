# M224 - Fluid Roto And RotoPaint Interaction

Deps: M146 - Roto Hosting For Any Node, M117 - GUI Perf Harness And Baseline (bench tooling).

Found during M146 - Roto Hosting For Any Node's UAT (user, 2026-10-26): strokes appear after a delay instead of under the pen, and roto editing feels heavy. Audit on main (50ee71b60): nothing draws the stroke while painting, so what the user sees is the render itself (`Engine/RotoPaintInteract.cpp:688`, `Engine/RotoCurveContext.cpp:1581`); stroke renders run on one thread (`Engine/OutputSchedulerThread.cpp:4304`); pen-up blocks the main thread (`Engine/RotoContext.cpp:499`); shape drags render only on release (`evaluateOnPenUp`, `Engine/RotoUndoCommand.cpp:181`); each motion event clones the dragged points into a new undo command; the overlay re-evaluates every Bezier on every paint (`Engine/RotoCurveInteract.cpp:2295`). This milestone measures, then puts a GPU preview under the pen, makes pen-up asynchronous, renders drags live, cuts per-event costs and closes the Nuke gesture gaps.

## Scope
**In:**
- A pen-to-pixel latency probe, a roto GUI bench, a stroke preview overlay, an async neat render, live drag renders, gesture-scoped undo capture, a cached and culled overlay, and Nuke gesture parity per the audit.
**Out:**
- Removing the GUI/internal double buffer (M163 - Incremental Bezier Edits kept it).
- Roto keys in the Dope Sheet and Curve Editor (M261 - Roto Keyframes In Dope Sheet And Curve Editor).
- Rewriting the stroke renderer itself (cairo); M64 - Tiled Rendering.
**Edge cases:**
- Multi-stroke mode keeps the preview until its last neat render lands.
- A new pen-down while the previous neat render is running must not trip the overlap assert (`Engine/ViewerInstance.cpp:833`).
- Eraser, clone and smear previews show the brush footprint only, not the result.
- A drag over a heavy graph must abort stale renders (latest wins).
**Assumptions:**
- The viewer's compatibility GL context allows the overlay to use vertex arrays.
- The finished stroke stays bit-identical because the neat render path is unchanged.

## Phase 224.1: Measure

- [ ] M224.P1.T1 — Add an opt-in pen-to-pixel latency probe
  - files: `Gui/ViewerGL.cpp`, `Engine/RotoPaintInteract.cpp`, `Engine/OutputSchedulerThread.cpp`, `Engine/ViewerInstance.cpp`
  - approach: when `NATRON_ROTO_LATENCY_LOG=<path>` is set, stamp `steady_clock` at the event (tablet/mouse), at `appendPoint`, at render request, start and end, at texture upload and at the `paintGL` swap. Write one JSON line per point age, on the same clock as M117's StallMonitor. When the variable is unset there is no cost beyond one branch.
  - verify: a GuiTests gtest drives five synthetic points and checks five records with monotonically increasing stamps. With the variable unset, no file is written.
  - size: M
- [ ] M224.P1.T2 — Add a roto bench driver to the GUI bench
  - files: `tools/bench/gui/roto_bench.py` (new), `tools/bench/gui/run.sh`, `tools/bench/gui/README.md`
  - approach: built on M117's `gui_bench.py`/`guitest.run()`. Ops: `stroke` at brush sizes 5/50/300 px, at 2K and 4K, with and without a `gen_graph` comp of 300 nodes upstream; `drag_point`, `drag_tangent`, `drag_feather` and `transform_box` on 1, 30 and 100 shapes of 24 points × 200 keys; `hover` over 100 shapes; `pen_up` stall. Each op reports p50/p95 pen-to-pixel latency (from P1.T1), overlay frame time and stalls.
  - verify: runs under `Tests/gui/run-gui-test.sh` and emits a record per op with no `timeout`. `compare.py --self-test` still passes.
  - size: M
- [ ] M224.P1.T3 — Record the Xvfb and host baselines and agree the budget
  - files: `tools/bench/BASELINE.md`, `tools/bench/gui/baselines/roto-xvfb.jsonl` (new)
  - approach: record an Xvfb baseline for regressions. Record host numbers from a stripped `release` AppImage on the user's workstation and on a modest-GPU machine. Rank the costs per stage from the probe; this ranking confirms or reorders Phases 2-4.
  - verify: the tables are recorded. The user signs off the latency budget, recorded in `## Decisions`.
  - size: M

## Phase 224.2: Strokes under the pen

- [ ] M224.P2.T1 — Draw the stroke being painted as a GPU overlay preview
  - files: `Engine/RotoPaintInteract.cpp`, `Engine/RotoPaintInteract.h`, `Engine/RotoCurveInteract.cpp`
  - approach: on each pen motion, append the raw sample (position, pressure) to an interact-owned buffer and request an overlay redraw only. `drawOverlay` draws pressure-scaled dabs or a quad strip in the brush colour × opacity with hardness falloff, adding only new segments each paint. The buffer clears when the neat render for that stroke age lands.
  - verify: a new Xvfb test, `Tests/gui/m224_stroke_preview.py`, drags on ViewerGL with renders held (viewer paused) and checks preview pixels at each sample position after one paint. Bench `stroke` p95 meets the budget from P1.T3.
  - size: L
- [ ] M224.P2.T2 — Coalesce stroke renders and let each one use the pool
  - files: `Engine/OutputSchedulerThread.cpp`, `Engine/RotoCurveContext.cpp`, `Engine/RotoStrokeItem.cpp`
  - approach: allow one stroke render in flight; requests that arrive meanwhile collapse into one, which picks up every point since the last rendered age (`getMostRecentStrokeChangesSinceAge` already accumulates). Keep stroke requests serialized but let each render use the thread pool for tiles in place of `maxThreads=1`. Remove the per-point `qDebug`.
  - verify: the stroke golden (a ≤256 px half-float crop) is bit-identical before and after. Bench `stroke` on 4K with heavy upstream shows the backlog flat (render requests ≤ points). Run `RotoSmear` and the existing roto tests.
  - size: L
- [ ] M224.P2.T3 — Make the pen-up neat render asynchronous
  - files: `Engine/RotoContext.cpp`, `Engine/RotoContext.h`, `Engine/RotoPaintInteract.cpp`, `Engine/ViewerInstance.cpp`
  - approach: replace the main-thread condition wait with a completion callback. The preview stays up until the neat render lands. A pen-down during a neat render either waits on a worker or queues behind it, never on the GUI thread, with no busy cursor.
  - verify: the Xvfb test draws two strokes 50 ms apart; StallMonitor records no stall over 50 ms; the final crop is bit-identical to the golden.
  - size: L

## Phase 224.3: Shape editing

- [ ] M224.P3.T1 — Render roto shapes live during drags
  - files: `Engine/RotoCurveInteract.cpp`, `Engine/RotoUndoCommand.cpp`
  - approach: point, tangent, feather and transform-box drags request a throttled, abortable `evaluateChange` (latest wins) instead of waiting for pen-up. Pen-up still triggers the final evaluate and autosave.
  - verify: `Tests/gui/m224_shape_drag.py` drags a point and samples a mask pixel that changes before release. Bench `drag_point` frame time is within budget.
  - size: M
- [ ] M224.P3.T2 — Capture undo state once per gesture
  - files: `Engine/RotoUndoCommand.cpp`, `Engine/RotoUndoCommand.h`, `Engine/RotoCurveInteract.cpp`
  - approach: Move, Transform, Tangent and FeatherBar commands snapshot the originals at gesture start (keyed by `UndoGesture`). Each motion event then applies only a delta to the open command, with no point clones and no merge.
  - verify: an undo/redo gtest over every drag type matches serialized roto byte for byte. Bench `drag_point` at 200 keys has flat per-event cost.
  - size: M
- [ ] M224.P3.T3 — Cache and cull the shape overlay
  - files: `Engine/RotoCurveInteract.cpp`, `Engine/Bezier.cpp`, `Engine/Bezier.h`
  - approach: cache the evaluated polygon and feather polygon per (Bezier, time) and invalidate them with the content hash. Skip shapes whose bbox lies outside the viewport, and draw from vertex arrays.
  - verify: overlay screenshots match before and after (Xvfb). Bench `hover` on 100 shapes drops and scales sublinearly.
  - size: M

## Phase 224.4: Gestures and modes

- [ ] M224.P4.T1 — Audit gesture parity against Nuke Roto/RotoPaint
  - files: `.plan` milestone `## Decisions` only
  - approach: build a table of every Nuke viewer gesture and shortcut in `.local/docs/nuke/.../rotopaint/` next to Natron's (`RotoCurveInteract.cpp:1210-1250`, `RotoPaintInteract.cpp:580-600`). Mark each row present, missing or conflicting.
  - verify: the user approves the adopt list.
  - size: M
- [ ] M224.P4.T2 — Add ripple-edit range modes
  - files: `Engine/RotoCurveInteract.cpp`, `Engine/Bezier.cpp`, `Engine/RotoCurveContext.cpp`, `Gui/ViewerGL.cpp`
  - approach: add modes all, from start, to end and range, as a choice next to the ripple toggle, and draw a red viewer border while ripple is on.
  - verify: a gtest ripples each mode on a 10-key shape. An Xvfb screenshot shows the border.
  - size: M
- [ ] M224.P4.T3 — Add the Bezier drawing and tangent modifiers
  - files: `Engine/RotoCurveInteract.cpp`, `Engine/RotoUndoCommand.cpp`
  - approach: implement the missing rows from P4.T1: Ctrl+click to sketch, Shift+click for a sharp exit point, Shift-drag to move both tangents, Ctrl-drag to break the tangent, Ctrl-drag to move the transform pivot.
  - verify: `Tests/gui/m224_gestures.py` performs each gesture with real keys and checks the control points.
  - size: M

## Phase 224.5: Host sign-off

- [ ] M224.P5.T1 — Rerun the roto bench on the host and do UAT
  - files: `tools/bench/BASELINE.md`
  - approach: rerun P1.T2 on the release AppImage on the workstation and the modest machine, and record the before/after table.
  - verify: the user signs off at UAT with screenshots.
  - size: M

**Verification gate:** `tools/ci/remote/build.sh release --tests` is green for each phase. The stroke and shape goldens are bit-identical. The `m224_*` Xvfb tests pass. `roto_bench` `compare.py` against `roto-xvfb.jsonl` shows no regression. Host p95 pen-to-pixel latency is within the signed-off budget, and the user passes UAT.

## Decisions

- 2026-10-28: GUI checks found a crash: painting while the viewer is paused rendered a live stroke from a null input (renderViewer_internal → getHash on null); a paused input now returns redraw with no args before consuming stroke points (gtest APausedViewerRendersNoLiveStroke). Python createEllipse/createRectangle now evaluate the roto. Open: plain S (cycle tool group) is also the global Project Settings shortcut — check a real key press in the viewer cycles the tool.
- 2026-10-28: Phase 224.4 implemented (build pending). Ripple range defaults 1–100, bounds in either order, red border drawn by the roto overlay. Ellipse/rectangle: Shift = square, Ctrl+Shift = from centre. Moved pivot resets on selection change, not undoable. Increase Feather (E) moves feather 10 screen px out; S cycles the current tool group; RotoPaint merge brush moved from E to D. Sketch (Ctrl+drag) keeps a point every ~12 screen px, smoothed, open shape. Ctrl during a draw drag latches for the drag. Not copied from Nuke: I (colour pick), T (onion skin; T shows the transform box), Ctrl+Shift corner pin (needs a new transform kind), B-spline tension (no B-spline tool).
- 2026-10-28: **P4.T1 gesture parity audit (awaiting user approval of the adopt list).** Source: Nuke 17 hotkeys (Roto/RotoPaint), "Using the Bezier tools", "Transforming", "Ellipse and Rectangle tools", "Rippling keyframes". Natron side: `RotoCurveInteract` toolbar/menu shortcuts, pen and key handlers; `RotoPaintInteract` pen-down.

  | Nuke | Natron before P4 | Status | Proposal |
  |---|---|---|---|
  | Backspace / Delete: delete points or shapes | Backspace "Remove points" (Delete matches) | present | keep |
  | C clone/reveal; N brush/eraser; X blur/sharpen/smear; V Bezier tools; Z smooth; Shift+Z cusp; Shift+E remove feather; Ctrl+A select all; Return close shape; T transform box/jack | same keys | present | keep |
  | D dodge/burn | D = points-edition tools; E = dodge/burn | conflicting | **adopt**: D = dodge/burn group; the points-edition group has no default key (rebindable) |
  | E increase feather on selected points | E = dodge/burn | conflicting | **adopt**: E = "Increase Feather" menu action (one step outwards along each point's feather direction) |
  | Esc back to the current Select tool | Esc only closes a Bezier being drawn | partial | **adopt**: Esc closes an open build, then returns to the last-used select tool |
  | S cycle the current tool's modes | none (Natron: repeat the group key) | missing | **adopt**: S cycles the active toolbar group |
  | I pick colour | none | missing | not copied now: needs a viewer colour pick into the roto colour knob; a separate feature |
  | T (clone) onion-skin source with jack | no onion skin | missing | not copied: onion skin is a feature, out of scope |
  | Ctrl+LMB Bezier tool: sketch | none | missing | **adopt** (T3) |
  | Shift+LMB Bezier tool: sharp exit on the previous point | none | missing | **adopt** (T3) |
  | Bezier tool drag: Shift both handles, Ctrl break | drag is always symmetric | partial | **adopt** (T3): Ctrl breaks (only the exit handle moves) |
  | Click the first point to close, then drag its tangents | closes, no drag | partial | **adopt** (T3) |
  | Shift+drag a tangent: both handles together | moves one (other keeps angle) | missing | **adopt** (T3) |
  | Ctrl+drag a tangent / Ctrl+LMB a point: break | Ctrl+drag breaks; Ctrl+click no-op | partial | **adopt** (T3): Ctrl+click breaks the point's handles |
  | Ctrl+drag jack centre: move pivot | host transform jack | present | keep |
  | Ctrl+drag transform-box centre: move pivot | none (pivot = box centre) | missing | **adopt** (T3); the pivot resets when the selection changes |
  | Ctrl+Shift+drag transform-box corner: corner pin | none | missing | not copied now: needs a per-point projective mapping in TransformUndoCommand; follow-up |
  | Shift+LMB add to transform box | same | present | keep |
  | Ctrl+Alt+LMB add point | same | present | keep |
  | Ellipse/Rectangle: Shift square, Ctrl+Shift from centre | Shift square, Ctrl from centre | conflicting | **adopt** Nuke mapping |
  | Shift+drag brush size; Ctrl+drag clone offset | same | present | keep |
  | Ctrl+Shift+drag B-spline tension | no B-spline tool | n/a | — |
  | Ripple: all / from start / to end / range, red viewer border | ripple toggle, all keys only, no border | partial | **adopt** (T2) |
  | Natron-only: Q select group, keypad nudge, Shift+L lock, Ctrl toggles box rotate/scale | — | extra | keep (no Nuke conflict) |

- 2026-10-28: Phases 224.2–224.3 implemented (builds pending). Preview: brush-faithful dabs into a destination-alpha mask; eraser/clone/smear show a 35% white footprint; the preview stays until the neat frame is displayed (1000 ms fallback if the frame never shows). One stroke render in flight (StrokeRenderCoalescer). Pen-up waits on no thread; a pen-down during a neat render is queued and replayed. Shape drags: preference "Roto Shape Drag Render" (Adaptive default, Always Live, On Release); Adaptive falls back to on-release above 100 ms per render; playback drags act as On Release. One undo entry per gesture (snapshot at start, delta per motion, exact redo from captured end state). Overlay: per-Bezier polygon cache keyed on time, content generation, GUI edit counter and transform; clip-space culling, selected shapes never culled.
- 2026-10-27 — **User:** preview under the pen = brush-faithful dabs (size, hardness, opacity, pressure). Live render while dragging shapes = a **preference** (Adaptive default: live with latest-wins abort, falling back to render-on-release when a frame exceeds a threshold; plus Always Live and On Release). Shortcuts: adopt **Nuke's Roto/RotoPaint map wholesale**. Latency budget: preview within **1 display frame p95**, real render converging within **100 ms**. Bench records keep the op's main cost in `median_ms` with a `headline` field (PM).
