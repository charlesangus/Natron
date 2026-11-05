# M339 - File Knob Reload Button Cleanup

Scene nodes keep showing two reload buttons (user, 2026-11-05): `KnobFile::showsReloadButton` defaults to true (`Engine/KnobFile.cpp:78`, icon at `Gui/KnobGuiFile.cpp:121`), and a node that adds its own Reload button has to remember `setShowsReloadButton(false)` too. Only `Engine/Nodes/IO/docs/NativeRead.md` says so. Nodes can't simply use the icon: `KnobFile::reloadFile()` ends in `eValueChangedReasonNatronInternalEdited`, which `knobChanged` can't tell from other internal edits, and it doesn't reload USD layers. This milestone makes the double button impossible rather than something to remember.

## Scope

**In:**
- One shared mechanism so a node can't show both controls, used by every node with a file knob and its own reload.
- A test over every registered node that fails if a node shows both.

**Out:**
- USDReference's own double button — fixed on M52 - USD Layer-Stack Completion; this milestone only moves it onto the mechanism.

**Edge cases:**
- NativeWrite's Reload is for its read-back and its file knob is a `KnobOutputFile`: not a double button, leave it.
- A file knob with no explicit button (DeepRead, USDLight, USDMaterial, LensDistortion, SchemaKnobMapper) keeps the icon.

## Phase 339.1: One reload control

- [ ] M339.P1.T1 — Shared file knob and reload mechanism
  - files: `Engine/KnobFile.{h,cpp}`, `Gui/KnobGuiFile.cpp`, `Engine/Nodes/Scene/USDReference.cpp`, `Engine/Nodes/Scene/USDCamera.cpp`
  - approach: choose between a helper that creates the file knob together with its Reload button and hides the icon, or routing the icon to a node reload hook so no node needs a second button. The second must give nodes a reason they can tell from other internal edits. Prove it on USDReference and USDCamera, which fixes USDCamera's double button (`Engine/Nodes/Scene/USDCamera.cpp:385-393`). Reload keeps rereading every sublayer through `SceneOps::reloadFileLayers`. Record the choice in `## Decisions`.
  - verify: `Reference3D_Test` and USDCamera's gtests pass; a test proves USDCamera's reload rereads a changed file.
  - size: L

- [ ] M339.P1.T2 — Move the remaining nodes onto it
  - files: `Engine/Nodes/Scene/USDRead.cpp`, `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/docs/NativeRead.md`, the OCIO file knobs (`git grep -l setShowsReloadButton Engine`)
  - approach: replace each hand-written `setShowsReloadButton(false)` plus button pair with the mechanism. Move the rule from `NativeRead.md` to the knob's header. Search `Gui` as well as `Engine` for explicit reload buttons.
  - verify: `git grep setShowsReloadButton` finds only the mechanism; those nodes' gtests pass.
  - size: M

- [ ] M339.P1.T3 — Python reload reaches the node's reload
  - files: `Engine/PyParameter.{h,cpp}` (`FileParam.reloadFile()`), Python tests
  - approach: `FileParam.reloadFile()` calls `KnobFile::reloadFile()`, which doesn't reload USD layers. Make it reach the node's reload on a knob using the mechanism, and leave other file knobs as they are.
  - verify: a Python test rewrites a referenced file, calls `reloadFile()` on a USDReference's file param, and sees the change.
  - size: M

## Phase 339.2: Keep it fixed

- [ ] M339.P2.T1 — Every registered node shows one reload
  - files: `Tests/` (new gtest beside the other plugin-walking specs, such as `Tests/Scene/ScenePrimPathKnobSpec_Test.cpp`), `Tests/gui/file_knob_reload.py` (new)
  - approach: the gtest creates every registered node and fails if one has a file knob showing its reload icon and an explicit reload button. The GUI script opens the panels of USDRead, USDReference, USDCamera and NativeRead and counts reload controls.
  - verify: the gtest fails when a `setShowsReloadButton`-style opt-out is removed from one node, and passes as committed; the GUI script shows one reload on each listed node.
  - size: M

**Verification gate:** release gate green with the new gtest; `Tests/gui/file_knob_reload.py` passes under Xvfb and shows one reload on each listed node.
