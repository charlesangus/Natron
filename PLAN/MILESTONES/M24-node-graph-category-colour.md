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
  - approach: Survey how Houdini, Nuke, Fusion and Blender separate "what kind of node is this" from "what colour did the user give it" in their network editors, Houdini especially. Then pin: the closed category list and each default colour; the user-border pen width in px at 100% zoom and how it scales; inset vs outset; the minimum border/body contrast rule; and how the border stays distinct from the selection halo (`_stateIndicator`, a `NodeGraphRectItem` at `depth-1` inflated by `NATRON_STATE_INDICATOR_OFFSET`, `Gui/NodeGui.cpp:697,1064-1067`). Nodes are square-cornered today (corner radius 0, `Gui/NodeGui.cpp:634`), since M18.P4.T4 removed the kind silhouettes. Also pin: (a) whether the Reader/Writer/Generator rungs beat the Deep group (DeepWrite is Writer-coloured today); (c) the label-text luminance threshold and the light/dark text colours. Items (b) 3D category and (d) edge width ladder are user decisions recorded in `## Decisions` — implement what they say.
  - verify: The note exists, names each surveyed application and what it does, and gives every number and rule above as one unambiguous value.
  - size: M

## Phase 24.2: Make the node category a first-class engine concept

- [ ] M24.P2.T1 — Add `NodeCategoryEnum` and a settings colour knob per category
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

- [ ] M24.P2.T2 — Resolve a node's category in one engine function
  - files: `Engine/Node.h`, `Engine/Node.cpp`, `Tests/NodeCategory_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Add `NodeCategoryEnum Node::getNodeCategory() const` with this ladder, in the order fixed by P1.T1: (1) Backdrop stays on its own colour — callers check first or a sentinel is returned; (2) `isReader()` / `isWriter()` / `isGenerator()`; (3) the major `PLUGIN_GROUP_*` from `getPluginGrouping()` through one static string→enum table — this covers every native node (all ~36 `NativePluginDescription`s set `grouping`) and the bundled OFX set; (4) a keyword heuristic over plugin label/ID for third-party OFX plugins with an arbitrary grouping; (5) `eNodeCategoryOther`. Do **not** add a `category` field to `NativePluginDescription` or touch any `Engine/Nodes/*` file: grouping already carries the information.
  - verify: A ctest case covers each rung: native Grade → color; DeepMerge → deep; Constant → generator; DeepWrite → whatever P1.T1 decided; TypedPassthrough → other; a test effect with grouping "Foo" and label "MyBlur" → filter via the heuristic. `tools/ci/local/test.sh ctest debug` green.
  - size: M

## Phase 24.3: Two colour channels on the node

- [ ] M24.P3.T1 — Route the GUI through `Node::getNodeCategory()` and delete the
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

- [ ] M24.P3.T2 — Split the node's single colour into category body + user border
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/NodeGraphRectItem.h`, `Gui/NodeGraphRectItem.cpp`
  - approach: Keep `_currentColor` as the category body applied by `applyBrush()` (`Gui/NodeGui.cpp:1782-1801`, which also covers `_nameFrame` and `_resizeHandle`). Add an optional `_userColor` drawn as an inset pen on `_boundingBox`, width/inset per P1.T1, inside the node footprint so it never reads as the `_stateIndicator` halo. Decide and document how it interacts with `_clonedColor` (`refreshCurrentBrush()`, `:1795`). The pen follows `_boundingBox`'s corner radius (0 today).
  - verify: Xvfb screenshots: a node with no user colour unchanged; a recoloured node with category body + user border; the same node selected with the halo still distinct; a cloned node unchanged.
  - size: L

- [ ] M24.P3.T3 — Persist "the user set a colour" explicitly
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

- [ ] M24.P3.T4 — Wire set and clear of the user colour through the panel and Python
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/DockablePanel.h`, `Gui/DockablePanel.cpp`
  - approach: Split `NodeGui::setCurrentColor()` (`Gui/NodeGui.cpp:3318`). The category path is called from `restoreStateAfterCreation()` (`:451`) and `setPluginIDAndVersion()` (`:3824`). The user path is the panel (`DockablePanel::onColorButtonClicked()` `:1455-1469` → `colorChanged` → `onSettingsPanelColorChanged` `:757`) and Python `NodeGui::setColor()` (`:3598-3606`). The panel button icon (`onColorDialogColorChanged` `:1380`) shows the user colour when set, otherwise the category colour. Add a "reset to category colour" action.
  - verify: Picking a colour in the panel sets the border and leaves the body; clearing removes the border; Python `setColor` sets the border; undo behaves as before. Xvfb screenshots of each.
  - size: M

- [ ] M24.P3.T5 — Re-colour open graphs when a category colour preference changes
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

- [ ] M24.P4.T2 — Remove the edge pen-width ladder
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
