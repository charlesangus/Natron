# M181 - Loop Drivers And UI

The user-facing side of loop drivers (user, 2026-10-24: both managed drivers and expressions). A right-click **Drive from loop** on any numeric knob inside a loop body turns the knob's panel row into an inline driver editor (start → end fields, a loop-variable picker, an easing button) that sets M180 - Loop Block Core's native `KnobDriver`: the loop variable (nearest loop's `loop.t` by default, or an outer loop's `loops.<name>.t`, `loop.iteration`, `loop.frame`), start and end values and the easing curve. LoopEnd's panel also lists every driven knob in its body in an editable table (user, 2026-10-25: inline and table). Driven knobs show their value at LoopEnd's single-pass iteration. The node graph marks loop blocks the way Houdini's network editor marks For-Each blocks. Power users can instead write Python expressions reading `loop.t` and friends through M71 - RFV Render Context's `rfv` object, exposed as a `loop` alias. Those are GIL-serialized and the docs say so.

## Scope

**In:**
- "Drive from loop" and "Remove loop driver" knob context-menu actions, the inline driver row, and LoopEnd's driven-knob table.
- Driven knobs display their value at LoopEnd's `singlePassIteration` (no separate preview knob).
- Node-graph block visuals matching Houdini's For-Each blocks (paired Begin/End shapes and colour, body marking, partner highlight on selection).
- `loop` / `loops` Python aliases over the context, for expressions.
- Undo for every driver edit.

**Out:**
- The driver engine itself: M180 - Loop Block Core.
- Making Python expressions fast (no exprtk-style engine): no milestone owns it.

**Edge cases:**
- "Drive from loop" on a knob outside any loop body is disabled with a tooltip saying why.
- A knob with an expression or animation: driving it replaces the knob's value source with the driver after a confirm; removing the driver restores the previous value source.
- A driver naming an outer loop that is later removed: the driver reads its own value and the knob shows a warning badge.
- Multi-dimension knobs: drive all dimensions together (default) or one dimension.
- The Make I-Node action opens LoopEnd's driver table empty (M189 - Make I-Node Action).

## Phase 181.1: Driver UI

- [x] M181.P1.T1 — Add the Drive from loop actions and the inline driver row
  - files: `Gui/KnobGui.cpp` (context menu), `Gui/KnobDriverRow.h`, `Gui/KnobDriverRow.cpp` (new), `Gui/CMakeLists.txt`
  - approach: The action finds the enclosing loop(s) by walking downstream from the knob's node to LoopEnds. The knob's row is replaced by start/end fields (prefilled: start = the knob's default, end = its current value), a variable picker over the enclosing loops' variables, all-dimensions vs one, and an easing button (linear, smooth, parametric curve widget). Start and end are animatable. Every edit is an undoable command.
  - verify: a GUI test (`Tests/gui/`) under Xvfb drives a Blur size inside a loop, undoes and redoes it, and checks the knob's driver state each time.
  - size: L

- [x] M181.P1.T2 — Add LoopEnd's driven-knob table and iteration display
  - files: `Gui/LoopDriverTable.h`, `Gui/LoopDriverTable.cpp` (new), `Engine/Nodes/Loop/LoopEnd.cpp`, `Gui/KnobGui.cpp`
  - approach: A table on LoopEnd's panel listing node, knob, variable, start, end and easing for every driven knob in the body, editable in place through the same undoable commands as the inline row. Driven knobs display their value at LoopEnd's `singlePassIteration`.
  - verify: the GUI test edits a driver from the table and sees the inline row update, and the displayed value changes with `singlePassIteration`; screenshots captured.
  - size: M

- [x] M181.P1.T3 — Draw loop blocks the way Houdini does
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGraph.cpp` (or the node paint code), `Tests/gui/m180_loop_blocks.py`
  - approach: First read Houdini's current docs on Block Begin and Block End nodes in the network editor and record what it draws (node shapes, the pair colour, how the body is marked, selection behaviour) in this file's `## Decisions`. Then implement the same: paired shapes and an auto-assigned shared colour for LoopBegin/LoopEnd, the body's marking, and partner highlighting on selection.
  - verify: the Xvfb GUI test builds a nested loop and screenshots it; the GUI bench (`tools/bench/gui/run.sh --xvfb`, `compare.py`) shows no regression.
  - size: M

- [ ] M181.P1.T4 — Driver Start/End keys in the curve editor, dope sheet and timeline
  - files: `Engine/KnobDriver*`, `Gui/CurveEditor*`, `Gui/DopeSheet*`, timeline key-marker tests
  - approach: Make Driver Start/End keys (Curve on KnobDriver, d3af7e02c) appear under their driven knob in the curve editor and dope sheet, editable there with undo, and marked on the timeline like knob keys.
  - verify: Add, move and delete Driver Start/End keys in both editors; the timeline markers and undo history stay synchronized.
  - size: M

- [ ] M181.P1.T5 — Knob expressions under knobChanged see preview loop variables
  - files: `Engine/EffectInstance.cpp`, `Engine/PyRFV.cpp`, `Engine/Knob.cpp`, loop expression tests
  - approach: `EffectInstance::onKnobValueChanged_public` sets analysis frame args before knobChanged; make "is this a render" mean a real render in one shared predicate used by `LoopEnd::evaluateStop`, PyRFV and knob expression context so analysis callbacks use `previewContextOf`.
  - verify: A body-knob expression reading `loop.iteration` from another knob's knobChanged sees the preview iteration.
  - size: M

## Phase 181.2: Expressions

- [x] M181.P2.T1 — Expose loop variables to Python expressions
  - files: `Engine/PyRFV.cpp`, `Engine/Knob.cpp` (`declarePythonVariables`), M71's expression read-set scan in `Engine/Knob.cpp`
  - approach: `loop` and `loops.<name>` are Python objects over the current context's `loop` / `loops.<name>` namespaces, like `rfv`. Extend the read-set scan to recognise `loop.<var>` and `loops.<name>.<var>`.
  - verify: gtest: an expression `loop.t * 10` inside a 3-iteration loop gives 0, 5, 10; its read-set names `loop.t` only.
  - size: M

- [x] M181.P2.T2 — Document drivers and expressions
  - files: `Engine/Nodes/Loop/docs/LoopEnd.md`, the user docs for expressions
  - approach: The editor, nesting names, the preview iteration, and the expression performance note (GIL-serialized; prefer drivers).
  - verify: reviewed at the gate.
  - size: S

## Decisions

- 2026-10-28 — Release gate green. Next: GUI checks/screenshots (container held for the M117/M180 benches), then AppImage + UAT.
- 2026-10-27 — PM: when a consumer names several contexts for an input (legacy LoopEnd), the input's actions (metadata, layers, identity) run under the last one, which is the iteration the task graph uses; a stop condition evaluated outside a render uses previewContextOf like knob expressions.
- 2026-10-27 — **Driver Start and End are animatable (user)**, like any knob: a driven value depends on the iteration and the time; an animated driver makes the loop output frame-varying.
- 2026-10-27 — PM: the block hull takes in side-branch nodes, matching what the driven-knob table and drivers count as the body. The pair colour comes from the loop's name and isn't saved; node bodies aren't tinted. On a partly driven multi-dimension knob, the undriven fields stay visible. Houdini's docs only say a hull is drawn; wedges and rings are ours.
- 2026-10-26: `loop.*` context variables stay plain values, never RFV bindings (M71 Phase 71.5).
- 2026-10-25 — **Inline row and LoopEnd table (user):** drivers are edited where the knob is and listed together on LoopEnd.
- 2026-10-25 — **No preview knob:** driven values display at LoopEnd's single-pass iteration, one control instead of two.
- 2026-10-25 — **Copy Houdini's block visuals (user).**
- 2026-10-25 — **Default driver range:** start = the knob's default (usually its identity: Blur size 0, Grade gain 1), end = its current value.

**Prior verification gate:** the full build and ctest pass; the Xvfb GUI tests pass; the GUI bench rule applies (`Gui/NodeGraph*`, `Gui/NodeGui*`); screenshots of the inline driver row, the driver table and the loop-block visuals are shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.

## Phase 181.3: UAT hull fix

- [ ] M181.P3.T1 — Keep the loop hull attached while the graph pans
  - files: `Gui/NodeGraph*`, loop-block hull drawing, loop GUI test
  - approach: Compute or invalidate hull geometry in the same scene/view coordinate system as the nodes so panning and zooming cannot leave the hull behind.
  - verify: The GUI test pans and zooms a loop graph and checks the hull continues to enclose the same nodes.
  - size: M

**Verification gate:** the original gate and GUI bench pass; the loop hull follows pan/zoom in the replacement AppImage.
