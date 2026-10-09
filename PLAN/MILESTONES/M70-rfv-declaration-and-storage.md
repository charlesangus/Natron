# M70 - RFV Declaration And Storage

Reverse-Flow Variables, part 1 of 3 (see `DECISIONS/2026-10-08-reverse-flow-variables-design.md`, which governs on any ambiguity). This milestone adds the way to *declare* variables: an RFV group knob, the `ReverseFlowVariableSet` node, and a project-level RFV group. It does not yet make variables flow or evaluate; that is M71 - RFV Render Context. The project already has no user-knob restore (`ProjectPrivate::restoreFromSerialization` matches knobs by name only), so that gap is closed here.

## Phase 70.1: The RFV group knob

- [ ] M70.P1.T1 — Add an RFV flag to the group knob and serialize it
  - files: `Engine/KnobTypes.h`, `Engine/KnobTypes.cpp` (KnobGroup), knob serialization under `Engine/` and `Serialization/`
  - approach: `KnobGroup::setAsRFVGroup(bool)` / `isRFVGroup()`, saved and restored with the group like the existing tab flag. Verify the real file names with a grep for `setAsTab` before editing. A group can only be flagged while it has no non-RFV-eligible children.
  - verify: a gtest creates a flagged group on a node, saves and reloads a project, and the flag survives.
  - size: M

- [ ] M70.P1.T2 — Add an RFV declaration accessor with name validation
  - files: `Engine/RFV.h`, `Engine/RFV.cpp` (new), `Engine/CMakeLists.txt`
  - approach: `RFV::getDeclaredVariables(KnobHolder*)` returns `(name, KnobIPtr)` for every direct child of the holder's RFV group. A name must be a valid Python identifier and unique within the holder. Children that are groups, pages, buttons or separators are rejected, because only value-holding knobs can be variables. Pure functions; no render state.
  - verify: gtest covers a valid list, a duplicate name, an invalid identifier and a rejected child type.
  - size: M

## Phase 70.2: The Set node

- [ ] M70.P2.T1 — Add the `ReverseFlowVariableSet` native node
  - files: `Engine/Nodes/RFV/ReverseFlowVariableSet.h`, `.cpp` (new), `Engine/AppManager.cpp` (one registration line), `Engine/CMakeLists.txt`
  - approach: model on `Engine/Nodes/TypedPassthrough.{h,cpp}`: polymorphic data kind in and out, `isIdentity` returns true with input 0, no `render()`. `initializeKnobs` creates a user page holding one RFV group. Read `Engine/Nodes/README.md` first. Grouping "Other" (or the closest existing one).
  - verify: a gtest creates the node, connects a Checkerboard through it and renders; the output is bit-identical to the input.
  - size: M

- [ ] M70.P2.T2 — Persist variables added to a Set node across save and load
  - files: `Tests/RFVSetNode_Test.cpp` (new), `Tests/CMakeLists.txt`; fix in `Engine/NodeGroup.cpp` / `Engine/Node.cpp` only if user knobs under the flagged group fail to restore
  - approach: add children (Double, String, Int2D, Bool, an animated Double) to the node's RFV group through `KnobHolder::create*Knob` + `recreateUserKnobs`, save the project, load it, and compare values and animation curves.
  - verify: the test passes; the reloaded node's `RFV::getDeclaredVariables` matches the saved list.
  - size: M

## Phase 70.3: The project RFV group

- [ ] M70.P3.T1 — Add a project RFV group and restore its user knobs on load
  - files: `Engine/Project.cpp` (`initializeKnobs`), `Engine/ProjectPrivate.cpp` (`restoreFromSerialization`), `Engine/ProjectSerialization.cpp`
  - approach: create an RFV group on the project settings. Serialization already writes persistent modified knobs; on restore, recreate the project's user knobs under the RFV group from the serialization *before* the by-name match. Mirror the `isUserKnob()` restore in `NodeGroup.cpp`.
  - verify: `Tests/ProjectSerialization_Test.cpp` gains a case: add three project variables (one animated), save, load into a fresh project, and values and curves match.
  - size: M

- [ ] M70.P3.T2 — Python entry point for declaring project variables
  - files: `Engine/PyAppInstance.h`, `Engine/PyAppInstance.cpp`, `Engine/PyNode.cpp` (reuse the `UserParamHolder` creators)
  - approach: expose `app.createRFV...Param(name, ...)` (or a restricted `UserParamHolder` over the project RFV group) for Int, Double, Bool, String, Choice, Color and the 2D/3D variants. Each creates the knob inside the project RFV group. Declaring a name that exists replaces nothing and raises. Because `declareCurrentAppVariable_Python` only declares knobs existing at that moment, rerun it after creation.
  - verify: a gtest runs a Python snippet that creates two project variables and reads them back through `app.getProjectParam`.
  - size: M

## Phase 70.4: GUI

- [ ] M70.P4.T1 — Restrict the user-parameter dialog around RFV groups
  - files: `Gui/ManageUserParamsDialog.cpp`, `Gui/ManageUserParamsDialog.h`
  - approach: on a node that has an RFV group, show the group as a pinned, undeletable entry, and allow adding value-knob children to it. The "Group" type on the add dialog never produces an RFV group, so only the Set node and the project carry one. A node with no RFV group cannot gain one.
  - verify: Xvfb GUI run (recipe in `build/deeprepro/run-gui.sh`): open a Set node, add a Double and a String variable, reload the project, and both are present. Take a screenshot of the dialog.
  - size: M

- [ ] M70.P4.T2 — Project settings variables editor
  - files: `Gui/ProjectGui.cpp`, `Gui/ProjectGui.h`, `Gui/ManageUserParamsDialog.cpp`
  - approach: a "Variables" section on the project settings panel that shows the RFV group's knobs, plus an "Edit variables..." button that opens `ManageUserParamsDialog` scoped to the project's RFV group.
  - verify: Xvfb GUI run: add a project variable from the panel, save and reload; screenshot of the panel.
  - size: M

**Verification gate:** ctest passes in full, including the new RFV tests; the Xvfb GUI run adds variables to a Set node and to the project and they survive a save/load; screenshots of both editors are shared with the user before sign-off.
