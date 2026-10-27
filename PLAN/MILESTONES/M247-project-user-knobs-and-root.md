# M247 - Project User Knobs And Root

Nuke keeps project settings as ordinary knobs on a root node. Users add their own knobs there
(show, shot, plate offset and so on) and drive expressions from them with `root.<knob>`. Natron
already has the substance of a root: `Project` is a `KnobHolder` and a `NodeCollection`
(`Engine/Project.h`), and its settings are knobs. Two things are missing. Users can't add their
own knobs to it, and there's no `root` name for expressions or scripts. This milestone adds both
and leaves `Project` as it is: it does not become a Node.

Code facts, read at `origin/main` on 2026-10-27. Re-check line numbers before starting.
- **Format.** The `ProjectSerialization` struct (`Serialization/ProjectSerialization.h`) has
  `projectKnobs` but no `userPages` or `pagesOrder`. Its YAML is in
  `Serialization/Yaml/ProjectYaml.cpp`, where `projectKnobs` is emitted at ~225 and read at ~285.
  Nodes already carry `userPages` in YAML (`Serialization/Yaml/NodeYaml.cpp` ~118/212, with
  `emitUserPages`/`readUserPages`).
- **Engine fill and restore.** The project save path is `Engine/ProjectSerializationConvert.cpp`
  (~389). It keeps knobs that are persistent and modified from their default. The node save path
  in the same file (~213–240) collects user pages through `userKnobLayoutToSerialization`.
  Project restore is in `Engine/ProjectPrivate.cpp` (~177–210). It only clones values onto
  project knobs that already exist and never creates one. Node restore is
  `Node::restoreUserKnobs` (`Engine/NodeMain.cpp` ~277), which recurses through
  `restoreUserKnobsRecursive`. It is Node-specific but builds on generic `KnobHolder` primitives:
  `getOrCreateUserPageKnob` (`Engine/Knob.cpp` ~7747), the `create*Knob` family, and
  `getUserPages`.
- **Python.** `class App` (`Engine/PyAppInstance.h` ~257) is a `Group`, not a `UserParamHolder`.
  `UserParamHolder` (`Engine/PyNode.h:102`) wraps any `KnobHolder*`, and its `create*Param`
  methods add knobs to the user page. `checkHolderIsEditable` (`Engine/PyParameter.cpp`) accepts
  non-effect holders. `app.<knob>` attributes are declared once per load by
  `AppInstance::declareCurrentAppVariable_Python` (`Engine/AppInstance.cpp` ~2446). The GUI
  override is in `Gui/GuiAppInstance.cpp`, which caches the declaration string. Nothing
  re-declares them when knobs change. The pattern to copy is `Node::declarePythonFields`
  (`Engine/Node.cpp`).
- **Expressions.** The prelude is `KnobHelperPrivate::declarePythonVariables`
  (`Engine/Knob.cpp` ~2930–2990). It always sets `app = <appID>` and `thisGroup`, and sets
  `thisNode`/`thisParam` only for EffectInstance holders. Dependencies come from a textual
  rewrite of `.get()`/`.getValue()`… calls into `.addAsDependencyOf(fromDim, thisParam, dim)`
  (~2990+). This rewrite never checks the holder type. A project knob's own expression has no
  `thisParam`, so its dependencies can't be tracked today. Renames rewrite expressions at
  ~3485–3508 and ~5625, and only generate the `thisGroup.`/`app.<node>.` forms.
- **GUI.** `DockablePanel::onManageUserParametersActionTriggered` (`Gui/DockablePanel.cpp`
  ~1635) returns early unless the holder is an EffectInstance. Most of `ManageUserParamsDialog`
  goes through `panel->getHolder()`. Its title and listener lookup are effect-specific, and
  `AddKnobDialog` has one EffectInstance cast. The project panel is a `DockablePanel` built in
  `Gui/ProjectGui.cpp`, and nothing on it opens the dialog.

## Scope

**In:**
- User pages and knobs on the Project: create, edit, reorder and delete them from the project
  settings panel's "Manage user parameters..." dialog and from Python, and save and load them in
  `.ntpz`.
- `root` as a Python name for the project. It's an alias of `app`, so `root.<knob>`,
  `root.getProjectParam()` and `root.create*Param()` all work. It's defined in the knob
  expression prelude and in the global script namespace (Script Editor, init scripts).
- Expression dependency tracking both ways: a node knob that expresses on a project knob
  updates when that knob changes, and a project knob may carry an expression on other knobs.
- When a project user knob is renamed, expressions that reference it are rewritten.

**Out:**
- Turning `Project` into a real Node or NodeGroup.
- Houdini-style network contexts.
- Inheriting project knobs down through groups. No milestone owns this.
- Backward compatibility. Files from before this milestone simply have no project user pages.

**Edge cases:**
- A user knob name that collides with a built-in project knob, a Python keyword, or an existing
  `App`/`Group` attribute (`getNodes`, `createNode`, …) is rejected at creation, in both the
  dialog and Python. The `app.<name>` attribute would otherwise shadow it or fail to declare.
- File > New, close and project reset remove project user knobs and their `app.<name>`
  attributes.
- A project user knob whose value is still its default still saves its definition.
  `userPages` carries the layout and value regardless of the modified-only filter on
  `projectKnobs`.
- An expression on a project knob that reads another project knob is the "root → root" case. It
  must not make a cycle the dependency code misses.

**Assumptions:**
- The listener mechanism (`KnobHelper::addListener`, `expressionChanged`) re-evaluates a node
  knob when a project knob it depends on changes, and the node's render hash moves. This hasn't
  been run. M247.P2.T2's test proves it, and a failure there becomes a fix inside that task.

## Phase 247.1: Format and engine

- [ ] M247.P1.T1 — Add `userPages` and `pagesOrder` to the project's serialization and YAML
  - files: Serialization/ProjectSerialization.h, Serialization/Yaml/ProjectYaml.cpp, Serialization/Yaml/ProjectYaml.h, Tests/ProjectSerialization_Test.cpp
  - approach: Give `ProjectSerialization` the same `std::list<GroupKnobSerialization> userPages` and `std::list<std::string> pagesOrder` fields `NodeSerialization` has. Emit and read them in ProjectYaml.cpp with the same helpers NodeYaml.cpp uses for nodes (`emitUserPages`/`readUserPages`). Expose those helpers from a shared header if they're file-static. Omit the keys when they're empty, as NodeYaml does. Update the format comment in ProjectYaml.h.
  - verify: A new pure-serialization test in Tests/ProjectSerialization_Test.cpp builds a `ProjectSerialization` with one user page that holds an int knob and a nested group with a string knob. It writes the YAML, reads it back and compares the structures. The existing ProjectSerialization tests still pass.
  - size: M

- [ ] M247.P1.T2 — Make the user-knob save and restore helpers generic over `KnobHolder`
  - files: Engine/ProjectSerializationConvert.cpp, Engine/NodeMain.cpp, Engine/NodePrivate.h, Engine/Knob.h, Engine/Knob.cpp
  - approach: Move `userKnobLayoutToSerialization` (ProjectSerializationConvert.cpp) and `Node::Implementation::restoreUserKnobsRecursive` plus the page loop in `Node::restoreUserKnobs` (NodeMain.cpp) into free functions or `KnobHolder` methods that take a `KnobHolder&`. Make the `Node` paths call them. Change no behaviour.
  - verify: Build, then pass the existing user-knob round-trip tests unchanged: Tests/ProjectSerialization_Test.cpp, plus any group/user-knob equivalence tests under Tests/Serialization/.
  - size: M

- [ ] M247.P1.T3 — Save and restore project user pages
  - files: Engine/ProjectSerializationConvert.cpp, Engine/ProjectPrivate.cpp, Engine/Project.cpp, Tests/ProjectSerialization_Test.cpp
  - approach: On save, fill `userPages` and `pagesOrder` from the project's user pages with the T2 helpers, and keep user knobs out of `projectKnobs`. On restore, build the user pages before the existing value-clone loop in ProjectPrivate.cpp, then restore their values, links and expressions the way NodeMain.cpp does for node user knobs. Make project reset (`Project::reset` and its callers) delete user knobs.
  - verify: A new gtest creates a user page on the project holding int, double, string, choice and bool knobs. One knob keeps its default value, and one has an expression on another. It saves a `.ntpz`, resets, loads, and checks that names, labels, values, the expression and the page order survive. After a reset, the project has no user knobs.
  - size: L

## Phase 247.2: Python and expressions

- [ ] M247.P2.T1 — Give `App` user params bound to the project, and keep the `app.<knob>` attributes current
  - files: Engine/PyAppInstance.h, Engine/PyAppInstance.cpp, Engine/typesystem_engine.xml, Engine/AppInstance.cpp, Engine/AppInstance.h, Gui/GuiAppInstance.cpp
  - approach: Make `App` also inherit `UserParamHolder`, with its holder set to the app's `Project`. Add it to the App entry in the shiboken typesystem, and use the `rejectNonEditableHolder` template the way UserParamHolder's own entry does. Add an `AppInstance::declareProjectPythonField(name)` / removal, modelled on `Node::declarePythonFields`, and call it whenever a project user knob is created, deleted or renamed, and after load restores user pages. Make the GUI override's cached declaration string stay in step, or drop the cache. Reject a name that is a Python keyword or already an `App` attribute, with a Python exception.
  - verify: A new gtest (Tests/ProjectUserParamsPython_Test.cpp, registered in Tests/CMakeLists.txt) runs Python through the app's interpreter: `app.createIntParam('shotNumber','Shot Number')`, then `app.shotNumber.set(12)`, and checks the knob value from C++. It also checks that `removeParam` drops the attribute and that `createIntParam('getNodes', …)` raises.
  - size: L

- [ ] M247.P2.T2 — Define `root` in scripts and expressions, and track project-knob dependencies
  - files: Engine/Knob.cpp, Engine/AppInstance.cpp, Gui/GuiAppInstance.cpp, Tests/ProjectUserParamsPython_Test.cpp
  - approach: Declare `root = app<N>` wherever `app<N>`/`app` is declared globally (`declareCurrentAppVariable_Python` and the GUI override), and add `root = <appID>` in `KnobHelperPrivate::declarePythonVariables`. When the holder is the Project, set `thisParam = <appID>.<knobName>` so a project knob's own expression can register dependencies. If the dependency test below shows that project-knob changes don't reach the listeners or move the node hash, fix that here.
  - verify: Extend the P2.T1 test file. A Blur's size expression `root.frameRange.get()[0]` and another `root.shotNumber.get() * 2` evaluate correctly. Changing either project knob re-evaluates them, and the Blur's render hash changes. A project user knob with expression `root.shotNumber.get() + 1` tracks shotNumber. A project expression cycle (a↔b) is reported as one, like a node cycle.
  - size: M

- [ ] M247.P2.T3 — Rewrite `root.`/`app.` references when a project user knob is renamed
  - files: Engine/Knob.cpp, Tests/ProjectUserParamsPython_Test.cpp
  - approach: Extend the rename-rewrite paths (Knob.cpp ~3485–3508 and ~5625) so that renaming a project knob rewrites `root.<old>` and `app.<old>` (and `app<N>.<old>`) in every dependent expression. Find the dependents through the knob's listeners, not by scanning the graph.
  - verify: A test gives a node knob the expression `root.shotNumber.get()`, renames shotNumber to shot, and checks that the expression now reads `root.shot.get()` and still evaluates.
  - size: M

## Phase 247.3: GUI

- [ ] M247.P3.T1 — Open "Manage user parameters..." on the project settings panel
  - files: Gui/DockablePanel.cpp, Gui/ManageUserParamsDialog.cpp, Gui/AddKnobDialog.cpp, Gui/ProjectGui.cpp
  - approach: Let `onManageUserParametersActionTriggered` accept a Project holder: skip the PyPlug warning and the node lookup for it. Remove the EffectInstance assumptions from ManageUserParamsDialog (title: "Project Settings"; the listener lookup) and AddKnobDialog (the cast at ~427). Validate a new knob's name against the project's knobs and App attributes, matching P2.T1's rule. Give the project panel the menu entry, and make sure its knob GUIs rebuild after the dialog edits them (`recreateUserKnobs`).
  - verify: Build. Then pass a new Xvfb GUI test (Tests/gui/m247_project_user_params.py, run through Tests/gui/run-gui-test.sh). It opens Project Settings, adds an Int "Shot Number" through the dialog, checks the knob shows on a user page in the panel, sets it, gives a Blur the expression `root.shotNumber.get()`, and checks the Blur's value. It saves screenshots of the dialog and of the panel.
  - size: L

**Verification gate:** run a full build and ctest, green, including the new ProjectSerialization
and ProjectUserParamsPython tests. Pass the m247 Xvfb GUI test, and send its screenshots to the
user for an OK before sign-off. Build a release AppImage for the user to test: add a project user
knob, drive a node from `root.<knob>`, then save, reopen and confirm both survive.

## Decisions

- 2026-10-27 — `root` aliases `app`; `Project` stays a non-Node: the project already serves as
  the root (KnobHolder + NodeCollection), and the format redesign removed the only serialization
  reason to convert it. `App` gains `UserParamHolder` bound to the Project, so `root.create*Param`,
  `root.<knob>` and `root.getNodes()` all work on one object, as `nuke.root()` does.
- 2026-10-27 — `root` resolves in both knob expressions and the global Python namespace (user).
