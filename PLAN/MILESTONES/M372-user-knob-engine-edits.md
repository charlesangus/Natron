# M372 - User Knob Engine Edits

The engine half of F16 - User Knob Editor. Today `AddKnobDialog` edits a user knob by deleting
it and creating a new one (`Gui/AddKnobDialog.cpp` ~1736-1970), nothing about user-knob
editing is undoable from the GUI, a knob's type is fixed once created, a user Button does
nothing unless the node's `onParamChanged` callback handles it, and the dialog can't create
the Path, ChannelSet, LayerSelect, ChannelSelect and Parametric knobs Python can
(`Engine/PyNode.h` `create*Param`). This milestone gives the engine an API that edits user
knobs in place, converts types, puts every operation on the global undo history, runs
per-button scripts and copies/pastes user-knob setups. M373 - In-Panel Knob Editor builds the
GUI on it.

Code facts, read at `origin/main` on 2026-11-08; re-check line numbers before starting.
- User knob flag: `KnobI::setAsUserKnob/isUserKnob` (`Engine/Knob.h` ~474). User page:
  `KnobHolder::getOrCreateUserPageKnob` (`Engine/Knob.cpp`, "User"/`userNatron`). Creation:
  `KnobHolder::create*Knob` (`Engine/Knob.h` ~2798-2816). Moves:
  `KnobHolder::moveKnobOneStepUp/Down` (`Engine/Knob.cpp` ~6244/~6316).
- Linked duplicates: `KnobHelper::createDuplicateOnHolder` (`Engine/Knob.cpp` ~5296).
- Snapshot undo already exists for Python edits: `UserKnobsCommand` and
  `PythonUserKnobsUndoGuard` (`Engine/PythonUndoCommands.cpp` ~878, ~1066), built on
  `PythonUserKnobs` (a before/after copy of the node's `userPages`). It is node-only.
- The graph recorder already emits one `SetUserKnobsOp` per user-knob change
  (`Engine/GraphEditRecorder.cpp` ~365-385), which live sessions replay.
- Serialization: `Serialization/KnobSerialization.h` holds `isUserKnob`, label, tooltip,
  newline, evaluates, and the per-type extras variant (choice, value range, file, path,
  layer select, text); YAML in `Serialization/Yaml/KnobYaml.cpp`.
- Rename rewrites dependents' expressions in `AddKnobDialog.cpp` ~1953 and, for project path
  variables, `RenamePathVariableCommand` (`Gui/PathVariableUndo.h`).

## Scope

**In:**
- An engine API that edits every user-knob property in place: script name, label, tooltip,
  hidden, new line, animates, render on change, ranges and display ranges, defaults, choice
  entries and help, text/file/path/layer flags, group-as-tab, parent page/group, position.
  Value, animation, expressions and links survive every edit.
- Type changes: int↔float, 1D/2D/3D↔each other, float 3D↔colour RGB↔RGBA carry values and
  curves over; any other change reports that it must reset, and resets only when told to.
- Every operation (create, delete, move, edit, type change, paste) is one entry on the global
  undo history, for node holders (EffectInstance, including Groups and PyPlugs).
- Button knobs carry a Python script, saved with the knob and run on click with
  `thisNode`/`thisParam` set; the node-level `onParamChanged` still fires.
- Every type Python can create is creatable and fully editable through the API and saves and
  loads as a user knob.
- Copy and paste of user knobs, groups and pages between holders as text.
- Python: button scripts and type change are reachable from Python.

**Out:**
- All GUI: M373 - In-Panel Knob Editor.
- User knobs on the Project: M247 - Project User Knobs And Root.
- Editing built-in (plugin) knobs.
- Backward compatibility with older projects.

**Edge cases:**
- Rename to a name used by another knob, a Python keyword, or (on a Group) a node inside it is
  rejected with a message; a valid rename rewrites dependents' expressions.
- Narrowing a type (3D→1D, RGBA→RGB) drops the extra dimensions' values and curves; widening
  fills new dimensions from the knob's default.
- Float→int rounds values and keyframes to the nearest integer.
- A type change keeps the knob's own expressions and links, and re-points other knobs'
  expressions and links at the new knob.
- Paste of a name already on the target gets a numeric suffix; expressions are pasted as text
  and may reference knobs that don't exist on the target (they error as any bad expression).
- Removing a knob that other knobs link to or express on keeps today's confirmation in the
  caller (GUI); the API reports the dependents.

**Assumptions:**
- `PythonUserKnobs`' snapshot captures values, curves, expressions and links fully enough to
  undo a type change and a delete. P1.T3's tests prove it.

## Phase 372.1: Edit API

- [ ] M372.P1.T1 — In-place user-knob edit API
  - files: new Engine/UserKnobEdit.h, new Engine/UserKnobEdit.cpp, Engine/Knob.h, Engine/Knob.cpp, Engine/KnobTypes.h, Engine/KnobTypes.cpp, Engine/CMakeLists.txt, new Tests/UserKnobEdit_Test.cpp, Tests/CMakeLists.txt
  - approach: Define a `UserKnobSpec` value type holding every editable property listed under Scope "In" (common fields plus a per-type variant mirroring `KnobSerialization`'s extras), with `specFromKnob(knob)` and `applySpec(holder, knob, spec)` that sets each changed property on the existing knob (setLabel, setHintToolTip, ranges, choice entries, ...), moves it to the spec's parent/index, and renames through the existing rename path so dependents' expressions are rewritten (move the rewrite out of AddKnobDialog.cpp ~1953 into this API; leave AddKnobDialog calling it). Validation (`validateSpec`) returns an error string for bad/duplicate names, Python keywords, a Group's inner node names and a choice default not in the menu. `createUserKnob(holder, spec)` creates via `create*Knob`. Type changes are T2.
  - verify: UserKnobEdit_Test: on a Blur, a user double with a keyframe, an expression, and another node's knob linked to it and expressing on it; apply a spec changing every property including the name; value, curve, expression and link survive and the dependent's expression is rewritten. Validation rejects each bad-name case.
  - size: L

- [ ] M372.P1.T2 — Type changes with value and curve conversion
  - files: Engine/UserKnobEdit.h, Engine/UserKnobEdit.cpp, Engine/Knob.cpp, Tests/UserKnobEdit_Test.cpp
  - approach: `changeUserKnobType(holder, knob, newType, allowReset)`: create the new knob beside the old with the same spec, copy values and curves per dimension for compatible pairs (Scope edge cases give rounding, narrowing and widening rules), move the knob's own expressions/links over, re-point every listener's expression or link to the new knob, delete the old knob and keep the position. For incompatible pairs return `needsReset` without changing anything unless `allowReset`.
  - verify: Tests: int(animated)→float keeps keys; float→int rounds; double3D→colour RGBA widens with the default alpha; RGBA→double narrows; a dependent node's expression and link still work after each change; text→int reports needsReset and changes nothing, then resets with allowReset.
  - size: L

- [ ] M372.P1.T3 — One undo entry per user-knob operation, on any node holder
  - files: Engine/PythonUndoCommands.cpp, Engine/PythonUndoCommands.h, Engine/UserKnobEdit.cpp, Tests/UserKnobEdit_Test.cpp
  - approach: Expose a `UserKnobsUndoScope(KnobHolder*, label)` built on `PythonUserKnobsUndoGuard`/`UserKnobsCommand` (keep it node-holder only; the Project comes with M247 - Project User Knobs And Root) and wrap `createUserKnob`, delete, move, `applySpec`, `changeUserKnobType` and paste in it, so each is one global undo entry with a readable label ("Add parameter size", "Change type of size"). Make sure a scope opened inside another scope (Python calling the API) records one entry, not two.
  - verify: Tests: each operation undoes and redoes to the exact prior state (values, curves, expressions, links, order), including a type change and a delete with a dependent; `GraphEditRecorder` records one `SetUserKnobsOp` per operation (pattern: Tests/GraphEditRecorder_Test.cpp `AddingAUserKnobGivesOneSetUserKnobs`).
  - size: M

## Phase 372.2: Types, buttons and copy/paste

- [ ] M372.P2.T1 — Per-button Python scripts
  - files: Engine/KnobTypes.h, Engine/KnobTypes.cpp, Serialization/KnobSerialization.h, Serialization/Yaml/KnobYaml.cpp, Engine/KnobSerializationConvert.cpp, Engine/PyParameter.h, Engine/PyParameter.cpp, Engine/typesystem_engine.xml, Engine/UserKnobEdit.h, Engine/UserKnobEdit.cpp, Tests/UserKnobEdit_Test.cpp
  - approach: Give `KnobButton` a `script` string, a `KnobButtonExtraSerialization` in the extras variant with YAML, a `UserKnobSpec` field, and `ButtonParam.setScript/getScript` in Python. On a user button's trigger, run the script through the app's interpreter with `thisNode`, `thisParam`, `thisGroup` and `app` set the way knob expressions declare them, before the node's `onParamChanged` callback; Python errors go to the Script Editor/log, never a crash.
  - verify: Tests: a user button whose script sets another user knob changes it when triggered; the script survives save and load; a script raising an exception leaves the app running and reports the error.
  - size: M

- [ ] M372.P2.T2 — Every Python-creatable type works through the spec and round-trips
  - files: Engine/UserKnobEdit.h, Engine/UserKnobEdit.cpp, Serialization/KnobSerialization.h, Serialization/Yaml/KnobYaml.cpp, Engine/KnobSerializationConvert.cpp, Tests/UserKnobEdit_Test.cpp
  - approach: Cover Path, ChannelSet, LayerSelect, ChannelSelect and Parametric in `UserKnobSpec` and `createUserKnob`/`applySpec`, including their type-specific properties (path multi-path, layer-select channel buttons, parametric curve count, curve labels and colours, range). Add any user-knob property that a type has but the extras variant doesn't save yet.
  - verify: A round-trip test creates one user knob of every type the Python `create*Param` list has, with non-default type-specific properties, saves a project, reloads it and compares `specFromKnob` before and after.
  - size: L

- [ ] M372.P2.T3 — Copy and paste user knobs as text
  - files: Engine/UserKnobEdit.h, Engine/UserKnobEdit.cpp, Tests/UserKnobEdit_Test.cpp
  - approach: `copyUserKnobs(holder, knobs)` serializes the chosen knobs, groups or pages (with their children) to the YAML `userPages` form; `pasteUserKnobs(holder, text, parent, index)` parses it, renames collisions with a numeric suffix, creates the knobs with values, curves and expression text, inside one `UserKnobsUndoScope`. Links to other nodes paste as links when the linked knob exists, else as unlinked copies.
  - verify: Tests: copy a page with a group and three knobs (one animated, one with an expression) from one Blur and paste onto another; definitions, values and curves match, a colliding name gets a suffix, and one undo removes the whole paste.
  - size: M

- [ ] M372.P2.T4 — Python type change
  - files: Engine/PyNode.h, Engine/PyNode.cpp, Engine/typesystem_engine.xml, Tests/UserKnobEdit_Test.cpp
  - approach: Add `UserParamHolder.changeParamType(param, typeName, allowReset=False)` over `changeUserKnobType`, returning the new Param (or None when a reset is needed and not allowed), and refreshing the node's Python attribute for the knob.
  - verify: A Python test changes an animated Int param to Double and reads its keys through the returned Param and `thisNode.<name>`.
  - size: M

**Verification gate:** full build and ctest green, including UserKnobEdit_Test and the existing user-knob, GraphEditRecorder and serialization tests. GUI bench per the board's GUI bench rule (the diff touches `Engine/Knob*`), result in `## Decisions`.

## Decisions

- 2026-11-08 — Edits apply in place; type changes convert compatible values and curves and ask before resetting otherwise (user, grill): keeps animation, expressions and links through edits.
- 2026-11-08 — One undo entry per operation (user, grill).
- 2026-11-08 — Buttons carry their own Python script; the node-level callback still fires (user, grill).
- 2026-11-08 — Every type Python can create is supported, with its type-specific properties (user, grill).
- 2026-11-08 — Copy/paste of user knobs between nodes is in; saved presets are not (user, grill).
- 2026-11-08 — Engine and editor are two milestones shipped together as F16 - User Knob Editor (user, grill).
