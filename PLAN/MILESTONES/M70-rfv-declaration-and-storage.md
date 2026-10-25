# M70 - RFV Declaration And Storage

Reverse-Flow Variables, part 1 of 3 (see `DECISIONS/2026-10-08-reverse-flow-variables-design.md`, which governs on any ambiguity). This milestone adds the way to *declare* variables: an RFV group knob, the `ReverseFlowVariableSet` node, and a project-level RFV group. It does not yet make variables flow or evaluate; that is M71 - RFV Render Context. The project already has no user-knob restore (`ProjectPrivate::restoreFromSerialization` matches knobs by name only), so that gap is closed here.

## Phase 70.1: The RFV group knob

- [x] M70.P1.T1 — Add an RFV flag to the group knob and serialize it
  - files: `Engine/KnobTypes.h`, `Engine/KnobTypes.cpp` (KnobGroup), knob serialization under `Engine/` and `Serialization/`
  - approach: `KnobGroup::setAsRFVGroup(bool)` / `isRFVGroup()`, saved and restored with the group like the existing tab flag. Verify the real file names with a grep for `setAsTab` before editing. A group can only be flagged while it has no non-RFV-eligible children.
  - verify: a gtest creates a flagged group on a node, saves and reloads a project, and the flag survives.
  - size: M

- [x] M70.P1.T2 — Add an RFV declaration accessor with name validation
  - files: `Engine/RFV.h`, `Engine/RFV.cpp` (new), `Engine/CMakeLists.txt`
  - approach: `RFV::getDeclaredVariables(KnobHolder*)` returns `(name, KnobIPtr)` for every direct child of the holder's RFV group. A name must be a valid Python identifier and unique within the holder. Children that are groups, pages, buttons or separators are rejected, because only value-holding knobs can be variables. Pure functions; no render state.
  - verify: gtest covers a valid list, a duplicate name, an invalid identifier and a rejected child type.
  - size: M

## Phase 70.2: The Set node

- [x] M70.P2.T1 — Add the `ReverseFlowVariableSet` native node
  - files: `Engine/Nodes/RFV/ReverseFlowVariableSet.h`, `.cpp` (new), `Engine/AppManager.cpp` (one registration line), `Engine/CMakeLists.txt`
  - approach: model on `Engine/Nodes/TypedPassthrough.{h,cpp}`: polymorphic data kind in and out, `isIdentity` returns true with input 0, no `render()`. `initializeKnobs` creates a user page holding one RFV group. Read `Engine/Nodes/README.md` first. Grouping "Other" (or the closest existing one).
  - verify: a gtest creates the node, connects a Checkerboard through it and renders; the output is bit-identical to the input.
  - size: M

- [x] M70.P2.T2 — Persist variables added to a Set node across save and load
  - files: `Tests/RFVSetNode_Test.cpp` (new), `Tests/CMakeLists.txt`; fix in `Engine/NodeGroup.cpp` / `Engine/Node.cpp` only if user knobs under the flagged group fail to restore
  - approach: add children (Double, String, Int2D, Bool, an animated Double) to the node's RFV group through `KnobHolder::create*Knob` + `recreateUserKnobs`, save the project, load it, and compare values and animation curves.
  - verify: the test passes; the reloaded node's `RFV::getDeclaredVariables` matches the saved list.
  - size: M

## Phase 70.3: The project RFV group

- [x] M70.P3.T1 — Add a project RFV group and restore its user knobs on load
  - files: `Engine/Project.cpp` (`initializeKnobs`), `Engine/ProjectPrivate.cpp` (`restoreFromSerialization`), `Engine/ProjectSerialization.cpp`
  - approach: create an RFV group on the project settings. Serialization already writes persistent modified knobs; on restore, recreate the project's user knobs under the RFV group from the serialization *before* the by-name match. Mirror the `isUserKnob()` restore in `NodeGroup.cpp`.
  - verify: `Tests/ProjectSerialization_Test.cpp` gains a case: add three project variables (one animated), save, load into a fresh project, and values and curves match.
  - size: M

- [x] M70.P3.T2 — Python entry point for declaring project variables
  - files: `Engine/PyAppInstance.h`, `Engine/PyAppInstance.cpp`, `Engine/PyNode.cpp` (reuse the `UserParamHolder` creators)
  - approach: expose `app.createRFV...Param(name, ...)` (or a restricted `UserParamHolder` over the project RFV group) for Int, Double, Bool, String, Choice, Color and the 2D/3D variants. Each creates the knob inside the project RFV group. Declaring a name that exists replaces nothing and raises. Because `declareCurrentAppVariable_Python` only declares knobs existing at that moment, rerun it after creation.
  - verify: a gtest runs a Python snippet that creates two project variables and reads them back through `app.getProjectParam`.
  - size: M

## Phase 70.4: GUI

- [x] M70.P4.T1 — Restrict the user-parameter dialog around RFV groups
  - files: `Gui/ManageUserParamsDialog.cpp`, `Gui/ManageUserParamsDialog.h`
  - approach: on a node that has an RFV group, show the group as a pinned, undeletable entry, and allow adding value-knob children to it. The "Group" type on the add dialog never produces an RFV group, so only the Set node and the project carry one. A node with no RFV group cannot gain one.
  - verify: Xvfb GUI run (recipe in `build/deeprepro/run-gui.sh`): open a Set node, add a Double and a String variable, reload the project, and both are present. Take a screenshot of the dialog.
  - size: M

- [x] M70.P4.T2 — Project settings variables editor
  - files: `Gui/ProjectGui.cpp`, `Gui/ProjectGui.h`, `Gui/ManageUserParamsDialog.cpp`
  - approach: a "Variables" section on the project settings panel that shows the RFV group's knobs, plus an "Edit variables..." button that opens `ManageUserParamsDialog` scoped to the project's RFV group.
  - verify: Xvfb GUI run: add a project variable from the panel, save and reload; screenshot of the panel.
  - size: M

## Phase 70.5: Moving variables between a Set node and the project

- [ ] M70.P5.T1 — "Existing variable" drop-down on the Set node
  - files: `Engine/Nodes/RFV/ReverseFlowVariableSet.cpp`, `Engine/RFV.cpp`, `Engine/RFV.h`
  - approach: a Choice knob outside the RFV group, entry 0 "Add existing variable...". Its entries are every variable name declared on the project or on any other Set node in the project, minus names already on this node, repopulated each time the node's panel refreshes. Choosing a name creates a child in this node's RFV group with the same knob type, dimension count and default as the first declaration found (project first), then resets the drop-down to entry 0. The new knob starts with the source's current value and no animation; it does not stay linked to the source.
  - verify: gtest: project declares `a` (Double) and another Set node declares `b` (String); on a third Set node, choosing `a` then `b` adds two matching children, and a name already present is not offered again.
  - size: M

- [x] M70.P5.T2 — `RFV::promoteToProject` operation
  - files: `Engine/RFV.cpp`, `Engine/RFV.h`, `Tests/RFVSetNode_Test.cpp`
  - approach: copies a variable knob from a Set node's RFV group into the project's RFV group (same type, dimensions, value, animation curves, label and hint). Refuses with a clear error if the project already declares that name. The node's own variable stays, so it keeps overriding the project value.
  - verify: gtest: promote an animated Double; the project knob's curve and value match, and a second promote of the same name fails. A gtest runs promote, undo and redo: undo removes the project's copy, redo restores it with the same value and animation.
  - size: M

- [x] M70.P5.T3 — Right-click "Add to project variables" on a Set-node variable
  - files: `Gui/KnobGui.cpp`, `Gui/KnobGui.h`
  - approach: the knob's context menu gains the item only when the knob is a direct child of a Set node's RFV group (not on the project's own group). Disabled, with a tooltip, when the project already declares the name. Calls `RFV::promoteToProject` as one undo step in M114's undo history. The node's own knob is never disabled, read-only or greyed out by a promote; only the menu item disables.
  - verify: Xvfb GUI run: right-click a variable on a Set node, choose the item, and see it appear in the project Variables section; screenshot of the menu. After the promote the node's knob is still enabled and editable (shown in a screenshot), and Ctrl+Z removes the variable from the project Variables section.
  - size: M

**Verification gate:** ctest passes in full, including the new RFV tests; the Xvfb GUI run adds variables to a Set node and to the project (including the drop-down and the right-click promote) and they survive a save/load; screenshots of both editors are shared with the user before sign-off.

## Decisions

- 2026-10-25 — **Existing-variable drop-down sits in its own group (user):** the "Add existing variable..." drop-down lives in a separate group on the Set node, apart from the node's own variables group, for picking global (project) variables.
- 2026-10-25 — **Promote is undoable; both copies stay active (user):** "Add to project variables" is one undo step (undo removes the project's copy, redo restores value and animation). After a promote neither copy is disabled, read-only or greyed; the node's copy keeps overriding the project's upstream of that Set node; the copies are not linked; only the menu item disables, with a tooltip. → DECISIONS/2026-10-25-rfv-promote-undo-and-active-copies.md
- 2026-10-25 — **Project variables and M114 undo (bosley-native-nodes):** (Amended 2026-10-25 by the user: "Add to project variables" is the exception, recorded as one undo step; see DECISIONS/2026-10-25-rfv-promote-undo-and-active-copies.md.) `app.createRFV*Param` is not recorded in the global undo history, matching M114's rule that project-level creation (addProjectLayer and the like) isn't; value edits to a variable are recorded through M114's per-knob path, and variables created on a Set node through the node API are recorded by M114's node path. A panel rebuild keeps its current user page (`DockablePanel::setPagesOrder` matches by page script name).
- 2026-10-25 — **Project variables from Python (bosley-native-nodes):** dedicated `app.createRFV<Type>Param(name, label)` methods with the UserParamHolder creators' arguments, raising ValueError on an existing, invalid or refused name, and on a name that is already an App attribute (a variable can't hide `render` or `createNode`). Variables are re-declared as `app.<name>` after a load and a reset. Reset deletes the project's variables.
- 2026-10-25 — **RFV group invariant both ways (bosley-native-nodes):** a flagged group refuses a non-variable child in addKnob/insertKnob, as setAsRFVGroup refuses while one exists; on load a refused child is logged and dropped.
- 2026-10-25 — **getDeclaredVariables failure mode (bosley-native-nodes):** a bad child is left out with a message, duplicate names reject every holder of the name, and every flagged group in the holder is read.
- 2026-10-25 — **Freshness (bosley-native-nodes):** the flag saves through the YAML format (`Serialization/Yaml/KnobYaml.cpp`, `ProjectSerializationConvert.cpp`) and restores in `NodeMain.cpp restoreUserKnobsRecursive`; native nodes register through `Engine/Nodes/*.reg.inc`; Engine sources need no CMake line. Later tasks naming `ProjectSerialization.cpp`, `NodeGroup.cpp` restore or `AppManager.cpp` registration read as those.
