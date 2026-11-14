# M424 - Python setExpression Refreshes The Viewer

User decision (2026-11-14, answering the open question raised under M311 - UDIM Live Textures): a Python `setExpression` must refresh the viewer, as an expression edit in the GUI does. Today `AnimatedParam::setExpression` (Python API) and the engine path it calls skip `evaluateValueChange()` on purpose, around `Engine/Knob.cpp` ~:4630 on main, so a script that sets an expression doesn't re-render, while the GUI's expression edit does.

## Scope

**In:**
- An expression set or cleared from Python evaluates the change as the GUI edit does: the node re-renders and viewers refresh. Param-changed callbacks fire with the same reason as the GUI path.
- Several sets inside a script still coalesce through the existing begin/end-changes or undo-group mechanisms, so each set doesn't trigger a separate render where the GUI path wouldn't.
- A gtest, plus a GUI check that the viewer updates after a Python `setExpression`.

**Out:**
- Changing how the GUI expression editor evaluates.
- Expression evaluation speed (F12 - Expression Speed).

**Edge cases:**
- Setting the same expression again: no extra render if nothing changed (the existing hash decides).
- `clearExpression` from Python: re-renders as well.
- Loading a project (expressions restored by deserialization) must not render once per knob; only interactive and script sets evaluate.
- Python `setExpression` on a knob of a node with no viewer: no error.

## Phase 424.1: Evaluate on Python expression sets

- [x] M424.P1.T1 — Python expression sets evaluate the change
  - files: `Engine/Knob.cpp`, `Engine/KnobImpl.h`, `Engine/ParameterWrapper.cpp` (Python `setExpression`/`clearExpression`), `Tests/` (gtest), `Tests/gui/` (script)
  - approach: find why the engine path skips `evaluateValueChange()` (the comment there) and how the GUI expression edit triggers evaluation. Make the Python path evaluate with the GUI's reason, keeping deserialization and internal restores silent. Respect begin/end-changes so a script's batch evaluates once.
  - verify: a gtest that a Python-style expression set on a node's knob bumps its hash and fires one evaluation; a GUI script where a viewer shows a Constant whose colour gets an expression from Python, and the viewer pixel changes without any other edit; existing expression tests pass.
  - size: M

**Verification gate:** the gtest and the GUI script pass in the remote gate. Project load renders no more often than before (no per-knob evaluations during deserialization). Existing expression and knob tests pass.

## Decisions
- 2026-11-14 — Python `setExpression` refreshes the viewer like the GUI (user).
- 2026-11-14 — P1.T1 implemented (4dbe2502f) at the Python API layer (`AnimatedParam::setExpression` evaluates after the engine call). The engine-level skip stays, since it protects deserialization, renames and the GUI's own evaluation. PM call: the callback reports userEdited False, like Python `setValue`, so scripts reacting to user edits don't recurse. Clearing is `setExpression("")`, as Python has no clearExpression binding. Gate running.
