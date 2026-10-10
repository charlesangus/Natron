# M24 - Node Graph Category Colour

Full title: Node graph aesthetics: category colour and user colour

Today a node's colour is chosen by `NodeGui::getColorFromGrouping()`
(`Gui/NodeGui.cpp:377`) from the plugin's major `PLUGIN_GROUP_*` string, and applied
as the node's whole body colour via `setCurrentColor()`. Two things are wrong with
that. First, the moment a user recolours a node the category signal is destroyed —
there is only one colour channel and the user's pick overwrites it. Second, the
category resolution is an ad-hoc `if/else` chain that is **duplicated verbatim** in
`Gui/ProjectGui.cpp:310-343`, where a fuzzy `> 0.05` RGB comparison against the
recomputed default is used to guess whether a serialized colour was user-chosen.

This milestone gives the node two independent colour channels — **body = category,
border = user** — makes the category a first-class engine concept that native nodes
register rather than a string match, and removes the two data-kind affordances that
M17 shipped and that have not earned their place: the tinted backdrop behind
deep/scene nodes, and the edge pen-width ladder.

Scope note (revised 2026-10-10): M18 Phase 18.4 already removed the node silhouettes, the tinted backdrop and the input-arrow glyphs, so there is no node-level kind visual left to protect; the edge colour and edge width are the only data-kind signals in the graph.

## Phase 24.1: Pin the visual specification

- [x] M24.P1.T1 — Survey the state of the art and pin the concrete visual spec
  - files: `PLAN/DESIGN/2026-09-07-node-graph-category-colour.md` (new, on the plan branch)
  - approach: Survey how Houdini, Nuke, Fusion and Blender separate "what kind of node is this" from "what colour did the user give it" in their network editors, Houdini especially. Then pin: the closed category list and each default colour; the user-border pen width in px at 100% zoom and how it scales; inset vs outset; the minimum border/body contrast rule; and how the border stays distinct from the selection halo (`_stateIndicator`, a `NodeGraphRectItem` at `depth-1` inflated by `NATRON_STATE_INDICATOR_OFFSET`, `Gui/NodeGui.cpp:697,1064-1067`). Nodes are square-cornered today (corner radius 0, `Gui/NodeGui.cpp:634`), since M18.P4.T4 removed the kind silhouettes. Also pin: (a) whether the Reader/Writer/Generator rungs beat the Deep group (DeepWrite is Writer-coloured today); (c) the label-text luminance threshold and the light/dark text colours. Items (b) 3D category and (d) edge width ladder are user decisions recorded in `

- [x] M24.P4.T3 — Stop fading optional-input edges
  - files: `Gui/Edge.cpp`
  - approach: Solid edges into optional inputs are drawn at 40% opacity (`Gui/Edge.cpp` ~:855), which dims a deep edge into the background now that colour is the only data-kind signal on edges. Draw them at full opacity like every other edge. Leave the dash patterns (mask, hidden and disconnected inputs) and the selection and highlight styling alone.
  - verify: In the screenshot run, the DeepFromImage → DeepMerge.A edge is the same blue as the other deep edges, and DeepToImage → Merge.B is the same black as the other image edges.
  - size: S

## Decisions` — implement what they say.
  - verify: The note exists, names each surveyed application and what it does, and gives every number and rule above as one unambiguous value.
  - size: M

## Phase 24.2: Make the node category a first-class engine concept

- [x] M24.P2.T1 — Add `NodeCategoryEnum` and a settings colour knob per category
  - files: `Global/Enums.h`, `Engine/Settings.h`, `Engine/Settings.cpp`
  - approach: Add `NodeCategoryEnum` beside `DataKindEnum` (`Global/Enums.h:531`)
    with the closed list fixed by M24.P1.T1 — at minimum read, write, generator,
    color, filter, channel, keyer, merge, draw, time, transform, views, deep, 3d,
    other. `Engine/Settings` already declares one `KnobColor` per existing group
    (`Engine/Settings.h:204-216,625-637`); **keep those knobs and their serialized
    `setName()` strings exactly as they are** so existing preferences keep loading,
    and add the missing ones — notably 3D, which has no knob today, so scene nodes
    currently fall through to `getDefaultNodeColor()`. Add a single
    `Settings::getNodeCategoryColor(NodeCategoryEnum, float* r, float* g, float* b)`
    that dispatches to the right knob, so callers stop hand-rolling the mapping.
  - verify: Builds; Preferences → Node Graph → Colors shows a colour entry for every
    category including 3D; an existing settings file written before this change
    still loads with its customised group colours intact.
  - size: M

- [x] M24.P2.T2 — Resolve a node's category in one engine function
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Tests/NodeCategory_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Add `NodeCategoryEnum Node::getNodeCategory() const` with this ladder, in the order fixed by P1.T1: (1) Backdrop stays on its own colour — callers check first or a sentinel is returned; (2) `isReader()` / `isWriter()` / `isGenerator()`; (3) the major `PLUGIN_GROUP_*` from `getPluginGrouping()` through one static string→enum table — this covers every native node (all ~36 `NativePluginDescription`s set `grouping`) and the bundled OFX set; (4) a keyword heuristic over plugin label/ID for third-party OFX plugins with an arbitrary grouping; (5) `eNodeCategoryOther`. Do **not** add a `category` field to `NativePluginDescription` or touch any `Engine/Nodes/*` file: grouping already carries the information.
  - verify: A ctest case covers each rung: native Grade → color; DeepMerge → deep; Constant → generator; DeepWrite → whatever P1.T1 decided; TypedPassthrough → other; a test effect with grouping "Foo" and label "MyBlur" → filter via the heuristic. `tools/ci/local/test.sh ctest debug` green.
  - size: M

## Phase 24.3: Two colour channels on the node

- [x] M24.P3.T1 — Route the GUI through `Node::getNodeCategory()` and delete the
      duplicated group chain
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/ProjectGui.cpp`
  - approach: Replace the `if/else` chain in `NodeGui::getColorFromGrouping()`
    (`Gui/NodeGui.cpp:377-430`) with `Node::getNodeCategory()` plus
    `Settings::getNodeCategoryColor()`, and rename it to something that says what it
    now does. Delete the verbatim duplicate of the same chain in
    `Gui/ProjectGui.cpp:310-343`. `Backdrop` keeps its own
    `getDefaultBackdropColor()` path — a backdrop is not a category.
  - verify: Builds; every node type that had a distinct colour before still gets the
    same colour, checked against the pre-change behaviour for one node per category.
  - size: M

- [x] M24.P3.T2 — Split the node's single colour into category body + user border
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/NodeGraphRectItem.h`, `Gui/NodeGraphRectItem.cpp`
  - approach: Keep `_currentColor` as the category body applied by `applyBrush()` (`Gui/NodeGui.cpp:1782-1801`, which also covers `_nameFrame` and `_resizeHandle`). Add an optional `_userColor` drawn as an inset pen on `_boundingBox`, width/inset per P1.T1, inside the node footprint so it never reads as the `_stateIndicator` halo. Decide and document how it interacts with `_clonedColor` (`refreshCurrentBrush()`, `:1795`). The pen follows `_boundingBox`'s corner radius (0 today).
  - verify: Xvfb screenshots: a node with no user colour unchanged; a recoloured node with category body + user border; the same node selected with the halo still distinct; a cloned node unchanged.
  - size: L

- [x] M24.P3.T2a — Make the user-colour border wrap the whole node, icon column included
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/NodeGraphRectItem.h`, `Gui/NodeGraphRectItem.cpp`
  - approach: P3.T2 strokes the inset border on `_boundingBox`, which covers only the coloured label area, so the dark plugin-icon column on the left (and anything else outside `_boundingBox`, such as the preview area) sits outside it. Stroke the border around the node's full footprint instead: the union of the icon column, `_boundingBox` and any preview. Either draw it on a dedicated item sized to that union, kept in sync wherever the node resizes or toggles its preview, or move the stroke to an item that already spans the whole node. The stroke stays inset: its outer edge sits on the full footprint, so it still never touches the outset `_stateIndicator` halo. Keep the contrast nudge, hiding on clones, and the Dot/Backdrop handling exactly as P3.T2 defined them.
  - verify: screenshot script (`build/m24-shots/m24_border.py`) shots of a user-coloured Grade show the border enclosing both the icon column and the label area, selected and unselected, with the halo still distinct; a node with its preview enabled shows the border around the whole node; no border on nodes without a user colour.
  - size: M

- [x] M24.P3.T3 — Persist "the user set a colour" explicitly
  - files: `Gui/NodeGuiSerialization.h`, `Gui/NodeGuiSerialization.cpp`,
    `Gui/ProjectGui.cpp`
  - approach: `NodeGuiSerialization` stores `_r/_g/_b` and a `_colorWasFound` flag,
    and `Gui/ProjectGui.cpp:347` guesses user intent with
    `std::abs(r - defR) > 0.05`. Replace the guess with truth: add a `HasUserColor`
    bool plus the user RGB, behind a serialization version bump using the existing
    versioned pattern (`Gui/NodeGuiSerialization.h:178-230`). On load of an older
    project, fall back to the current 0.05 comparison **once** to seed the flag, so
    existing projects keep their user colours. Delete the comparison from the
    current-version path.
  - verify: Save a project with one recoloured node and one untouched node, reload,
    and confirm the recoloured node keeps its border and the untouched node picks up
    the current category colour (including after the category colour is changed in
    Preferences). A project saved before this change still loads with its recoloured
    nodes recoloured.
  - size: L

- [x] M24.P3.T4 — Wire set and clear of the user colour through the panel and Python
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/DockablePanel.h`, `Gui/DockablePanel.cpp`
  - approach: Split `NodeGui::setCurrentColor()` (`Gui/NodeGui.cpp:3318`). The category path is called from `restoreStateAfterCreation()` (`:451`) and `setPluginIDAndVersion()` (`:3824`). The user path is the panel (`DockablePanel::onColorButtonClicked()` `:1455-1469` → `colorChanged` → `onSettingsPanelColorChanged` `:757`) and Python `NodeGui::setColor()` (`:3598-3606`). The panel button icon (`onColorDialogColorChanged` `:1380`) shows the user colour when set, otherwise the category colour. Add a "reset to category colour" action.
  - verify: Picking a colour in the panel sets the border and leaves the body; clearing removes the border; Python `setColor` sets the border; undo behaves as before. Xvfb screenshots of each.
  - size: M

- [x] M24.P3.T5 — Re-colour open graphs when a category colour preference changes
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`, `Gui/NodeGraph.cpp`, `Gui/NodeGui.cpp`
  - approach: `Settings::onKnobValueChanged` already emits `settingChanged(KnobI*)` (`Engine/Settings.cpp:2257`). Add `Settings::isNodeCategoryColorKnob(KnobI*)`; the node graph connects to `settingChanged` and on a category knob re-applies the category body colour and label contrast (P3.T6) to every `NodeGui`, leaving user borders untouched. Skip while settings are being restored (`_restoringSettings`).
  - verify: With a graph open, changing the Merge colour in Preferences updates every Merge node's body and label colour live; a recoloured Merge node keeps its border.
  - size: M

- [ ] M24.P3.T6 — Contrast-aware node label colour (fixes deep nodes' black-on-navy labels)
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`
  - approach: The label is hard-coded black at `Gui/NodeGui.cpp:682` and `:3203` (`setNameItemHtml()`, `:3107-3222`). When the label HTML has no user `<font color>`, pick light or dark text from the relative luminance of the body colour (category colour, or `_clonedColor` for clones) using P1.T1's threshold. A user font colour from `KnobGuiString::parseFont` still wins. Re-evaluate whenever the body brush changes (`applyBrush`/`refreshCurrentBrush`). Deep's default (0, 0, 0.38) must give white text.
  - verify: Xvfb screenshots: white label text on DeepRead/DeepMerge (navy), dark text on a Grade (light), a label with an explicit `<font color>` unchanged; with P3.T5, a live Preferences change flips the text.
  - size: M

## Phase 24.4: Retire the data-kind leftovers

- [ ] M24.P4.T1 — Remove the dead NodeGui kind-tint helper left behind by M18
  - files: `Gui/NodeGui.cpp`
  - approach: M18.P4.T3/T4 already removed the tinted backdrop, silhouette radius and input glyphs; `NodeGui::paint()` (`:2264`) is empty. Delete the now-unused static `kindTintColor()` and its comment (`Gui/NodeGui.cpp:181-201`). Fix the comment at `:1354-1355` so it no longer mentions a silhouette; keep the `update()` call only if something still reads kind at paint time, otherwise delete it with the comment. Leave `Gui/Edge.cpp`'s own `kindTintColor()` alone.
  - verify: Builds with no unused-function warning; `grep -n kindTintColor Gui/NodeGui.cpp` returns nothing.
  - size: S

- [x] M24.P4.T2 — Remove the edge pen-width ladder
  - files: `Gui/Edge.cpp`
  - approach: Run only if `## Decisions` says the ladder goes. Delete `kindWidthMultiplier()` (`Gui/Edge.cpp:785-797`) and its use at `:830` so every edge uses `EDGE_PEN_WIDTH`. Keep the Okabe-Ito colours (`:800-813`, `:880`). Rewrite the comment at `:779-782` to say colour is now the only kind channel. Leave the dash pattern for mask/hidden inputs alone.
  - verify: Xvfb screenshot of a graph with image and deep edges: same width, deep edges still blue, mask/hidden dash and selection/highlight unchanged.
  - size: M

## Decisions

- 2026-09-07 — Body colour carries the category, a thick border carries the user's
  colour: chosen over a left category spine and over the inverse (user body,
  category border). Keeps today's appearance for the large majority of nodes that
  are never recoloured, and gives the recolour affordance its own channel rather
  than letting it destroy the category signal.
- 2026-09-07 — Closed `NodeCategoryEnum` rather than a user-editable taxonomy: the
  categories are already implicit in `PLUGIN_GROUP_*` and in the per-group colour
  knobs `Engine/Settings` has carried for years, so a fixed-but-recolourable list
  needs no new serialized data model, no list-editor widget, and no rule for what
  happens to nodes whose category was deleted. Studio-specific remapping of
  third-party OFX plugins is deliberately deferred.
- 2026-09-07 — The M17 edge pen-width ladder is removed as not noticeable, making
  the Okabe-Ito edge colour the sole edge-level kind channel. This supersedes the
  rationale documented in `Gui/Edge.cpp:780-783`; node silhouette shape remains the
  non-colour channel for data kind.

- 2026-10-10 — **Freshness check at promotion (§5a):** M18 Phase 18.4 had already done P4.T1's job (P4.T1 is now a dead-code cleanup), and every native node from M67 already sets a `PLUGIN_GROUP_*` grouping, so P2.T2 resolves the category from grouping instead of adding a `NativePluginDescription` field. P1.T1, P3.T2, P3.T4–T6 and P4.T2 were rewritten against current code; P3.T6 now carries the 2026-10-03 deep-label fix and is sized M. P2.T1, P3.T1 and P3.T3 stand, but their line numbers have moved: settings getters are at `Engine/Settings.h:200-218`, knob creation at `Engine/Settings.cpp:922-1012`, `getColorFromGrouping` at `Gui/NodeGui.cpp:385-437`, and the duplicate chain at `Gui/ProjectGui.cpp:304-351`. P3.T3 also covers `NodeGui::copyFrom()` (`Gui/NodeGui.cpp:2321-2326`, the paste/preset path). The third 2026-09-07 decision's "node silhouette shape remains the non-colour channel" is no longer true.

- 2026-10-10 — **Edge width ladder goes (user):** M24.P4.T2 runs even though, with the silhouettes gone, edge colour becomes the graph's only data-kind signal.
- 2026-10-10 — **Two 3D categories (user):** add both a USD 3D category and a Native 3D category, each with its own Preferences colour knob, although no scene nodes exist yet. P1.T1 gives each a default colour; P2.T1 adds both enum values and knobs.

- 2026-10-10 — **P2.T2 verify:** full debug ctest 1266/1267. The one failure, `DeepChannelNodesTest.DeepRemoveLayersIsRegisteredAsADeepNode`, passed its assertions and then crashed (SIGSEGV) on a worker thread during teardown; it passed 5/5 when re-run alone. It is an intermittent teardown crash, not caused by this change. Callers check `isBackdropNode()` before `getNodeCategory()`, since there is no backdrop sentinel in `NodeCategoryEnum`.

- 2026-10-10 — **GUI evidence needs the `fast` build:** debug builds trap FP exceptions (`App/NatronApp_main.cpp`, under `DEBUG`), and llvmpipe trips the trap at GL context creation, so Natron dies with SIGFPE under Xvfb (this is M25 - GL Init FP Guard). Screenshots use `build/fast/App/Natron` and the script at `build/m24-shots/m24_border.py` (untracked), run through `Tests/gui/run-gui-test.sh` with `GUI_TEST_TIMEOUT=120`.
- 2026-10-10 — **P3.T2 / P4.T2 evidence:** shots confirm uniform edge width (2 px core), deep edges blue, image edges black, the halo distinct from the node, and a clone's body in the clone colour. The user border has no Python path until P3.T4, so its shot waits for that task. The same run surfaced three things to look at:
  - Deep nodes are narrower than other nodes, with no icon column, so their labels overflow the box. P3.T6 fixes the label contrast but not the overflow; node-text wrap is M56's.
  - Solid optional-input edges draw at 40% opacity, which dims a deep edge into the background now that colour is the only kind signal.
  - Natron's SIGTERM handler calls `quitApplication()` from the signal handler and often does not exit, so a hung GUI test outlives its timeout.

- 2026-10-10 — **P3.T3:** NodeGuiSerialization version 6 → 7 (`NODE_GUI_INTRODUCES_USER_COLOR`), with GuiTests `NodeGuiUserColorSerialization.*` (4/4). The GUI save/reload round-trip moves to the gate, since nothing outside C++ sets a user colour until P3.T4. Older Natron builds cannot read v7 projects, as with every earlier bump. A pre-existing bug was found but left alone: `NodeGui::copyFrom()` swaps green and blue when restoring the overlay colour (`fromRgbF(overlayR, overlayB, overlayG)`).

- 2026-10-10 — **P3.T4 evidence:** the screenshot script passed all checks.
  - Python `setColor` draws the red inset border, which stays distinct from the white selection halo.
  - A user colour within 0.02 of the body is nudged lighter, so it stays visible.
  - `resetColor()` restores the category colour, and the user colour survives save and reload.
  - The reset is a right-click on the panel's colour button. Colour changes stay off the undo stack, as before.
  - Headless `setColor` is still a no-op, the same as position and size.
  - **For the gate:** the border wraps only the coloured label area (`_boundingBox`), not the dark icon column on the left. Confirm whether it should wrap the whole node.

- 2026-10-10 — **Border wraps the whole node (user):** answers the P3.T4 gate note. The border must enclose the icon column too, so P3.T2a was added.

- 2026-10-10 — **One build at the end (user):** builds are the bottleneck on this host, so tasks are implemented and committed without per-task builds or tests, and implementers do not compile. The milestone gets a single build, test and screenshot run once every task has landed, and any breakage is fixed then. Task checkboxes mean implemented and committed, not verified.

- 2026-10-10 — **Optional inputs are not faded (user):** this answers the P3.T2 evidence note. P4.T3 was added.

**Verification gate:** `format`, `lint-ci` and `build-and-test` green; plus visual
evidence captured the same way M17's node-graph evidence and M23's packaging gate
were captured (Xvfb + screenshot, since this cannot be asserted in a unit test),
covering: one node per category showing its category colour; a recoloured node
showing category body plus user border; that same node selected, showing the border
and the selection halo are still tellable apart; a deep node and a scene node showing
their silhouettes with no colour outside the shape; and a graph with deep, scene and
image edges all at uniform width. Plus a round-trip check that a project saved before
this milestone loads with its recoloured nodes still recoloured.
- 2026-10-03 — **Deep nodes draw black label text on dark navy in the node graph** (found in M60's GUI pass, predates M60). User decision: fix it here, with the node colour rework, not on the M60 branch. Add a task when M24 is elaborated/started.
