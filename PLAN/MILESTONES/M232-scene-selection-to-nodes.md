# M232 - Scene Selection To Nodes

Turn a 3D selection into node work, the Solaris/Nuke way:
- A picker button on every prim path/pattern knob, which picks from a scene-graph popup or
  takes the current selection.
- "Set selection on current node".
- "Create node from selection", through the standard tab menu scoped to nodes that accept
  a scene.

From the M19/M20 UAT feedback (2026-10-26). Builds on M231 - Scene Graph Panel (tree
model, drag hint on path knobs) and M228 - 3D Viewer Navigation (selection set). Survey:
`PLAN/DESIGN/2026-10-26-3d-mvp-survey.md`.

## Scope

**In:**
- A picker button beside each knob flagged as a prim path/pattern (M231.P2.T2's hint). It opens a popup scene-graph tree over the node's *input* scene, with a "Use viewer selection" entry. A path knob takes one prim, and a pattern knob takes many, joined with spaces.
- "Set selection on current node": a 3D view and Scene Graph panel action with a rebindable shortcut. It writes the selection into the pattern/path knob of the node whose settings panel is current. When that node has several such knobs, the first pattern knob wins.
- "Create node from selection". The 3D view and Scene Graph panel open the standard tab menu filtered to node types that accept a scene input and have a prim pattern/path knob. The chosen node is created downstream of the viewed node, with its pattern pre-filled with the selection.
- The tab menu's kind filtering is a general mechanism. Launching it from a context that knows the upstream kind hides nodes whose inputs can't accept that kind.

**Out:**
- Filtering unrelated launch contexts beyond the selected node's output kind and scene-selection workflows.
- Python access to the viewer selection — none.

**Edge cases:**
- No current node, or a current node with no path/pattern knob: the action is disabled, with a status-bar hint.
- Selection paths that don't exist in the current node's input scene (the node is upstream of a re-parent): the paths are still written, and the node's zero-match warning shows. No rewriting.
- An empty selection: "Create node from selection" still opens the filtered menu and leaves the pattern empty.
- Create-from-selection when the viewed node already has outputs: the new node is inserted between the viewed node and its first scene output, as the graph's insert does.

## Phase 232.1: Knob picker

- [ ] M232.P1.T1 — Prim picker button on path and pattern knobs
  - files: `Gui/KnobGuiString.{h,cpp}`, `Gui/ScenePrimPickerPopup.{h,cpp}` (new, reusing M231's `SceneGraphModel`), `Gui/CMakeLists.txt`, `Tests/gui/scene_prim_picker.py`
  - approach: knobs carrying the prim-path hint get a small button. The popup evaluates the node's input 0 scene (through `SceneQueryPool`) and shows a checkable tree (single-select for a path knob, multi for a pattern knob) with OK/Cancel and a "Use viewer selection" button. OK writes the knob as one undo step.
  - verify: Xvfb GUI script — on a USDEditTransform, pick two prims in the popup: the pattern becomes `"/a /b"`; "Use viewer selection" writes the 3D view's set; Cancel changes nothing.
  - size: M

## Phase 232.2: Selection actions

- [ ] M232.P2.T1 — Set selection on the current node
  - files: `Gui/SceneViewport.cpp`, `Gui/SceneGraphPanel.cpp`, `Gui/ActionShortcuts.cpp` (3DViewer binding), `Gui/ViewerTab.cpp` (current-node lookup), `Tests/gui/scene_set_on_node.py`
  - approach: a shared helper finds the node whose settings panel is current (the same notion M20's gizmo used, "panel opened last"), takes its first prim-pattern knob (else its first prim-path knob), and writes the selection as one undo step. Expose it in both context menus and as a bound action.
  - verify: Xvfb GUI script — with a USDPrune panel open, select two prims in the viewer and press the shortcut: the pattern holds both and the prims vanish; with an image node's panel open, the action is disabled.
  - size: M

- [ ] M232.P2.T2 — Kind-filtered tab menu and create-from-selection
  - files: `Gui/TabMenuBridge.{h,cpp}`, `Gui/Resources/TabTabTab/tabtabtab_natron.py`, `Tests/python/test_tabtabtab_natron.py`, `Gui/SceneViewport.cpp`, `Gui/SceneGraphPanel.cpp`, `Tests/gui/scene_create_from_selection.py`
  - approach: `TabMenuBridge::launch` gains an optional context (an upstream data kind, a required-knob hint and a completion callback). The Python adapter filters the item list to plugins whose input 0 accepts that kind and that declare a prim pattern/path knob. Expose the plugin's accepted kinds and hint to Python through the existing plugin query, adding a field if one is missing. On selection, create the node, insert it after the viewed node, and pre-fill its pattern. The node-graph launch is unchanged.
  - verify: python test — with a scene context the filtered list contains USDEditTransform, USDPrune and USDMaterial and no Blur/Grade; Xvfb GUI script — create-from-selection of USDPrune with two prims selected inserts it downstream of the viewed node with the pattern set and the prims pruned in the view.
  - size: M

**Prior verification gate:** release gate green; picker, set-on-node and create-from-selection GUI scripts pass under Xvfb; screenshots of the picker popup and the filtered tab menu shared before sign-off.

## Phase 232.3: UAT selection and typed-menu fixes

- [ ] M232.P3.T1 — Always write the selected prim pattern
  - files: create-from-selection and set-on-current-node helpers, scene selection GUI tests
  - approach: Populate the target node's first compatible prim pattern/path for both actions before the first evaluation, preserving multi-selection ordering and one undo step.
  - verify: Viewer and Scene Graph actions create or update a node with the exact selected paths already filled in.
  - size: M

- [ ] M232.P3.T2 — Wrap selected prims with USDXformPrim
  - files: USDXformPrim creation action, scene graph rewiring, selection GUI tests
  - approach: When USDXformPrim is created from a selected prim, insert a new Xform at that location and reparent the selected prim and descendants beneath it while keeping the graph connection in the selected node's place.
  - verify: A selected hierarchy is wrapped, not duplicated; child paths and downstream scene output remain correct after undo/redo.
  - size: L

- [ ] M232.P3.T3 — Filter the normal Tab menu by selected output kind
  - files: `Gui/TabMenuBridge.*`, TabTabTab Python adapter/tests, node-graph launch context
  - approach: Pass the selected node's output kind to ordinary node-graph Tab launches and show only nodes with at least one compatible input, while an empty selection keeps the full menu.
  - verify: DeepImage selection shows DeepImage consumers and hides image/scene-only nodes; Scene and Image cases have equivalent coverage.
  - size: M

- [ ] M232.P3.T4 — Lay out variadic typed inputs like Merge and Switch
  - files: USDMerge, USD transform and RFVSwitch input metadata, node-edge layout tests
  - approach: Show the two active/default inputs in normal positions, hide unused variadic inputs on the left, and reveal the next input for manual connection without changing multi-connect gestures.
  - verify: The three node families show compact edge layouts and still accept bulk/multi-input connections.
  - size: M

**Verification gate:** all selection and Tab-menu tests pass; patterns, wrapping, typed filtering and compact input layouts are accepted in the replacement AppImage.
