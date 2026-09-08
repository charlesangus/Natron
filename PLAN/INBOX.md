## Pending

### 2026-09-07T20:35:24-04:00 — change-request
- refs: M24, M17
- Add a new milestone **M24 — Node graph aesthetics: category colour and user colour**. Create `PLAN/MILESTONES/M24-node-graph-category-colour.md` with the content below, and add a board row `| M24 | Node graph aesthetics: category colour and user colour | todo | [M24-node-graph-category-colour.md](PLAN/MILESTONES/M24-node-graph-category-colour.md) |` at the end of the `# Board` table.
- **Do not start M24 until M17 has merged.** M24 removes the edge pen-width ladder that M17 shipped (`kindWidthMultiplier()` in `Gui/Edge.cpp`), so starting it while M17's PR is open would put the two milestones in conflict over the same lines.
- Also record this project-wide decision (new file `PLAN/DECISIONS/<date>-node-colour-carries-category.md` + INDEX line): *Node body colour carries the node's category; a user-chosen colour is carried by a thick border instead of replacing the body. Data kind is signalled by node silhouette shape and edge colour only — the edge pen-width ladder introduced in M17 is removed as not noticeable in practice, superseding M17's "width is the primary channel, colour is reinforcement only" rationale in `Gui/Edge.cpp`.* Decided by the user 2026-09-07 during `/cat-plan`.

Full content for `PLAN/MILESTONES/M24-node-graph-category-colour.md`:

---8<--- BEGIN MILESTONE FILE ---8<---
# Milestone 24: Node graph aesthetics: category colour and user colour

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

Scope note: this milestone does **not** touch the node silhouette shapes
(`kindSilhouetteCornerRadiusPx()`) or the input-arrow glyph shapes and tints
(`Gui/NodeGui.cpp:1660-1690`). Those are working and stay exactly as they are.

## Phase 24.1: Pin the visual specification

- [ ] M24.P1.T1 — Survey the state of the art and pin the concrete visual spec
  - files: `PLAN/DESIGN/2026-09-07-node-graph-category-colour.md` (new)
  - approach: Survey how Houdini, Nuke, Fusion and Blender separate "what kind of
    node is this" from "what colour did the user give it" in their network editors —
    Houdini especially, since its node graph is the reference. Then pin the numbers
    this milestone builds to, so the GUI tasks are not making them up: the closed
    category list and each category's default colour; the user-border pen width in
    px at 100% zoom and how it scales; whether the border is drawn inset or outset
    relative to the node rect; the minimum contrast rule between border and body so
    a user colour close to the category colour is still visible; and what the border
    must look like so it is never confused with the selection halo
    (`_stateIndicator`, a `NodeGraphRectItem` at `zValue depth-1` inflated by
    `indicatorOffset`, `Gui/NodeGui.cpp:714,1076`). Record any judgement call as a
    milestone-scoped decision in `## Decisions` below.
  - verify: The note exists, names each surveyed application and what it does, and
    states every number above as a single unambiguous value that a later task can
    implement without further design work.
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

- [ ] M24.P2.T2 — Let nodes declare their category, with a heuristic fallback
  - files: `Engine/Nodes/NativeEffectBase.h`, `Engine/Nodes/NativeEffectBase.cpp`,
    `Engine/Nodes/TypedPassthrough.cpp`, `Engine/Node.h`, `Engine/Node.cpp`
  - approach: Add a `NodeCategoryEnum category` field to `NativePluginDescription`
    (`Engine/Nodes/NativeEffectBase.h`) — the same place a native node already
    declares its id, grouping, inputs and `outputKind` — defaulting to
    `eNodeCategoryOther`, and set it on `TypedPassthrough`'s descriptor (the only
    native node today). Add `Node::getNodeCategory()` resolving in a fixed ladder:
    (1) the declared category if the effect is a `NativeEffectBase`; (2)
    `isReader()` / `isWriter()` / `isGenerator()`; (3) the major `PLUGIN_GROUP_*`
    grouping string — this is what covers the bundled OFX set, which declares proper
    groupings; (4) a keyword heuristic over the plugin label and ID for third-party
    OFX plugins whose grouping is arbitrary, in the spirit of Nuke's name matching;
    (5) `eNodeCategoryOther`. Keep the ladder in one function in the engine so no
    caller ever re-derives it.
  - verify: A unit or manual check that each rung fires — `TypedPassthrough` resolves
    via its descriptor, a bundled OFX Grade resolves to the color category via its
    grouping, and a plugin with a nonsense grouping but a recognisable name resolves
    via the keyword heuristic rather than falling to other.
  - size: L

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
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/NodeGraphRectItem.h`
  - approach: `NodeGui` currently keeps one `_currentColor` that is both the category
    default and the user's pick. Keep the category colour as the body brush applied
    to `_boundingBox` (`Gui/NodeGui.cpp:1864`) and add an explicit optional user
    colour: when set, draw it as a border pen on `_boundingBox` itself, to the width
    and inset fixed by M24.P1.T1. Draw it **inside the node's own footprint** — not
    as an inflated outer rect — because `_stateIndicator` already owns the inflated
    halo for selection and state, and an outer border would be indistinguishable
    from it. The pen must follow `_boundingBox`'s current corner radius so it works
    on the capsule and rounded silhouettes too.
  - verify: A node with no user colour looks the same as before this milestone. After
    setting a user colour, the body keeps the category colour and the border shows
    the user colour; selecting that node still shows a visually distinct selection
    halo.
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

- [ ] M24.P3.T4 — Wire set and clear of the user colour through the panel
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/DockablePanel.cpp`
  - approach: `NodeGui::setCurrentColor()` (`Gui/NodeGui.cpp:3407`) is called both to
    apply a category default (`restoreStateAfterCreation()`,
    `setPluginIDAndVersion()`) and to apply a user pick (the panel colour button,
    `Gui/DockablePanel.cpp:1463`). Separate them: the default path sets the category
    colour, the panel path sets the user colour. The colour button must show the
    user colour when one is set and the category colour otherwise, and there must be
    a way to clear the user colour and return the node to its category colour.
  - verify: Picking a colour in the panel sets the border and leaves the body alone;
    clearing it removes the border; both are undoable if the existing colour change
    was undoable.
  - size: M

- [ ] M24.P3.T5 — Re-colour open graphs when a category colour preference changes
  - files: `Engine/Settings.h`, `Engine/Settings.cpp`, `Gui/NodeGraph.cpp`,
    `Gui/NodeGui.cpp`
  - approach: Changing a category colour in Preferences should recolour every
    already-placed node of that category, not just newly created ones. Hook the
    category colour knobs' value-changed path in `Engine/Settings` and have the node
    graph re-apply the category colour to each `NodeGui` that has no user colour set.
    Nodes with a user colour keep their border and also update their body, since the
    body is still the category channel.
  - verify: With a graph open, change the Merge category colour in Preferences and
    watch every merge node's body update without reopening the project.
  - size: M

## Phase 24.4: Retire the data-kind affordances that did not earn their place

- [ ] M24.P4.T1 — Remove the tinted backdrop behind deep and scene nodes
  - files: `Gui/NodeGui.cpp`
  - approach: `NodeGui::paint()` (`Gui/NodeGui.cpp:2336`) fills the whole bounding
    rect with an opaque kind tint that `_boundingBox` then covers except at the
    corners, so all the user ever sees is coloured wedges bleeding out of the node's
    rounded corners. Delete that fill. Keep the `_boundingBox->setCornerRadiusPx()`
    call in the same function — the silhouette shape is the affordance that works.
    Keep `kindTintColor()`, which the input-arrow glyphs still use
    (`Gui/NodeGui.cpp:1676-1684`); if `paint()` ends up with nothing left but the
    corner-radius update, move that update somewhere it is not doing a full repaint's
    work.
  - verify: A deep node renders as a clean capsule and a scene node as a clean
    rounded rect, with no colour visible outside the silhouette at any zoom level;
    the input-arrow glyphs are unchanged.
  - size: S

- [ ] M24.P4.T2 — Remove the edge pen-width ladder
  - files: `Gui/Edge.cpp`
  - approach: Delete `kindWidthMultiplier()` (`Gui/Edge.cpp:785`) and its use at
    `Gui/Edge.cpp:830` so every edge is drawn at `EDGE_PEN_WIDTH` regardless of data
    kind — in practice the 3x/2x difference is not noticeable. Keep the Okabe-Ito
    kind colour (`Gui/Edge.cpp:800,880`): it is now the only edge-level kind channel,
    which is acceptable because Okabe-Ito is chosen to be distinguishable under all
    common colour-vision deficiencies, and node shape carries the same information
    redundantly. Update the comment block at `Gui/Edge.cpp:780-783`, which currently
    asserts the opposite ("width is the primary channel ... colour is reinforcement
    only, never the sole signal"), so the file does not document a rule it no longer
    follows.
  - verify: All edges render at the same width; deep edges are still blue and scene
    edges still orange; selection and highlight states, which are orthogonal to kind
    styling, still render as before.
  - size: S

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

**Verification gate:** `format`, `lint-ci` and `build-and-test` green; plus visual
evidence captured the same way M17's node-graph evidence and M23's packaging gate
were captured (Xvfb + screenshot, since this cannot be asserted in a unit test),
covering: one node per category showing its category colour; a recoloured node
showing category body plus user border; that same node selected, showing the border
and the selection halo are still tellable apart; a deep node and a scene node showing
their silhouettes with no colour outside the shape; and a graph with deep, scene and
image edges all at uniform width. Plus a round-trip check that a project saved before
this milestone loads with its recoloured nodes still recoloured.
---8<--- END MILESTONE FILE ---8<---
