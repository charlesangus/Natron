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

- [ ] M181.P1.T1 — Add the Drive from loop actions and the inline driver row
  - files: `Gui/KnobGui.cpp` (context menu), `Gui/KnobDriverRow.h`, `Gui/KnobDriverRow.cpp` (new), `Gui/CMakeLists.txt`
  - approach: The action finds the enclosing loop(s) by walking downstream from the knob's node to LoopEnds. The knob's row is replaced by start/end fields (prefilled: start = the knob's default, end = its current value), a variable picker over the enclosing loops' variables, all-dimensions vs one, and an easing button (linear, smooth, parametric curve widget). Start and end are animatable. Every edit is an undoable command.
  - verify: a GUI test (`Tests/gui/`) under Xvfb drives a Blur size inside a loop, undoes and redoes it, and checks the knob's driver state each time.
  - size: L

- [ ] M181.P1.T2 — Add LoopEnd's driven-knob table and iteration display
  - files: `Gui/LoopDriverTable.h`, `Gui/LoopDriverTable.cpp` (new), `Engine/Nodes/Loop/LoopEnd.cpp`, `Gui/KnobGui.cpp`
  - approach: A table on LoopEnd's panel listing node, knob, variable, start, end and easing for every driven knob in the body, editable in place through the same undoable commands as the inline row. Driven knobs display their value at LoopEnd's `singlePassIteration`.
  - verify: the GUI test edits a driver from the table and sees the inline row update, and the displayed value changes with `singlePassIteration`; screenshots captured.
  - size: M

- [ ] M181.P1.T3 — Draw loop blocks the way Houdini does
  - files: `Gui/NodeGui.cpp`, `Gui/NodeGraph.cpp` (or the node paint code), `Tests/gui/m180_loop_blocks.py`
  - approach: First read Houdini's current docs on Block Begin and Block End nodes in the network editor and record what it draws (node shapes, the pair colour, how the body is marked, selection behaviour) in this file's `## Decisions`. Then implement the same: paired shapes and an auto-assigned shared colour for LoopBegin/LoopEnd, the body's marking, and partner highlighting on selection.
  - verify: the Xvfb GUI test builds a nested loop and screenshots it; the GUI bench (`tools/bench/gui/run.sh --xvfb`, `compare.py`) shows no regression.
  - size: M

## Phase 181.2: Expressions

- [ ] M181.P2.T1 — Expose loop variables to Python expressions
  - files: `Engine/PyRFV.cpp`, `Engine/Knob.cpp` (`declarePythonVariables`), M71's expression read-set scan in `Engine/Knob.cpp`
  - approach: `loop` and `loops.<name>` are Python objects over the current context's `loop` / `loops.<name>` namespaces, like `rfv`. Extend the read-set scan to recognise `loop.<var>` and `loops.<name>.<var>`.
  - verify: gtest: an expression `loop.t * 10` inside a 3-iteration loop gives 0, 5, 10; its read-set names `loop.t` only.
  - size: M

- [ ] M181.P2.T2 — Document drivers and expressions
  - files: `Engine/Nodes/Loop/docs/LoopEnd.md`, the user docs for expressions
  - approach: The editor, nesting names, the preview iteration, and the expression performance note (GIL-serialized; prefer drivers).
  - verify: reviewed at the gate.
  - size: S

## Decisions

- 2026-10-26: `loop.*` context variables stay plain values, never RFV bindings (M71 Phase 71.5).
- 2026-10-25 — **Inline row and LoopEnd table (user):** drivers are edited where the knob is and listed together on LoopEnd.
- 2026-10-25 — **No preview knob:** driven values display at LoopEnd's single-pass iteration, one control instead of two.
- 2026-10-25 — **Copy Houdini's block visuals (user).**
- 2026-10-25 — **Default driver range:** start = the knob's default (usually its identity: Blur size 0, Grade gain 1), end = its current value.

**Verification gate:** the full build and ctest pass; the Xvfb GUI tests pass; the GUI bench rule applies (`Gui/NodeGraph*`, `Gui/NodeGui*`); screenshots of the inline driver row, the driver table and the loop-block visuals are shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.
