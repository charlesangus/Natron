### 2026-08-30T10:21:32+00:00 — info
- refs: M2.P1.T2h
- Pause after the current task. Finish M2.P1.T2h (or whatever task is in flight when this is read), commit/checkpoint as normal, then set board `status: paused` and stop — do not start the next task until told to resume.

### 2026-08-30T23:56:05-04:00 — info
- refs: M8
- User asks: pause after M8 (Branching model and CI/CD rebuild) closes — do not start the next milestone (M2 resume, M3, M5, or M6) without checking in first. Finish out M8's remaining tasks and its gate normally, then set `current: null`, leave the board row `done`, and stop.

  **Processed 2026-08-31T00:54:39-04:00.** Honoured: M8 closed, board went to
  `current: null`, and the run stopped. The user then checked in and chose to
  resume M2, so this entry is discharged.

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
## Pending

### 2026-09-18T01:31:24-04:00 — change-request
- refs: M24
- Append a new trailing task to M24's Phase 24.3 (after M24.P3.T5), covering
  the backlog item "node text should flip to white if the node colour is dark
  so text remains readable." Insert into
  `PLAN/MILESTONES/M24-node-graph-category-colour.md`:

  ```
  - [ ] M24.P3.T6 — Flip node label text colour to white when the node body
        colour is dark
    - files: `Gui/NodeGui.cpp`, `Gui/NodeGui.h`
    - approach: Node labels are drawn in a fixed colour today. Compute the
      relative luminance of the node's current body (category) colour
      wherever the label is painted/updated, and switch between a light and a
      dark text colour at a fixed luminance threshold, so labels stay
      readable regardless of category or user body colour. Re-evaluate when
      the body colour changes at runtime (the M24.P3.T5 re-colour path), so
      an open graph's label colours stay correct after a Preferences change.
    - verify: Xvfb GUI check — a node with a light category colour keeps dark
      text, a node with a dark category colour shows white text, and
      changing a category's colour in Preferences updates already-placed
      nodes' label colour along with their body.
    - size: S
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M33)
- New milestone. Suggested file: `PLAN/MILESTONES/M33-input-pipe-visibility.md`.
  Board row: `| M33 | Node graph: input pipes not always shown | todo | ... |`

  ```
  # Milestone 33: Node graph — input pipes not always shown

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Users report that a connected input sometimes fails to show its pipe/edge in
  the node graph. Investigate root cause (a repaint/refresh gap, z-order issue,
  or a specific node type/action that triggers it) and fix so every connected
  input reliably draws its pipe.

  Blocked on: not yet reproduced — needs a concrete repro (which node types or
  actions trigger the missing pipe) before this can be elaborated.

  Acceptance sketch:
  - A repro case is identified and documented.
  - Every connected input reliably shows its pipe across the reproduced
    scenario and a general graph smoke-test.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M34)
- New milestone. Suggested file: `PLAN/MILESTONES/M34-new-native-shuffle-node.md`.
  Board row: `| M34 | New native Shuffle node | todo | ... |`

  ```
  # Milestone 34: New native Shuffle node

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  The existing Shuffle node's UI/semantics are counter-intuitive and reportedly
  broken. Design and ship a new native Shuffle node with clearer
  channel-mapping semantics, replacing (or living alongside, then replacing)
  the current one.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a design pass on the new node's UI/semantics before
  elaboration.

  Acceptance sketch:
  - A native Shuffle node ships with clear, testable channel-mapping
    semantics.
  - Existing projects using the old Shuffle node still load (migration or
    compatibility path defined).
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M35)
- New milestone. Suggested file:
  `PLAN/MILESTONES/M35-remove-implicit-shuffle-elsewhere.md`. Board row:
  `| M35 | Remove implicit output-plane shuffling from non-Shuffle nodes | todo | ... |`

  ```
  # Milestone 35: Remove implicit output-plane shuffling from non-Shuffle nodes

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Nodes other than Shuffle currently expose an "output plane" selector that
  effectively lets them shuffle channels. Drop that from all non-Shuffle
  nodes so they process channels in-place, and point users to the Shuffle
  node for actual shuffling.

  Blocked on: M34 — needs the new native Shuffle node shipped as the
  supported replacement path before removing the escape hatch elsewhere.

  Acceptance sketch:
  - Non-Shuffle nodes no longer expose an output-plane/channel-shuffle
    selector.
  - Channel remapping is only available via the Shuffle node.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M36)
- New milestone. Suggested file: `PLAN/MILESTONES/M36-new-channel-affordance.md`.
  Board row: `| M36 | Add "new channel/layer" affordance wherever a node outputs channels | todo | ... |`

  ```
  # Milestone 36: Add "new channel/layer" affordance wherever a node outputs channels

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Anywhere a node has a channel/layer output setting, add a "new" option to
  create a channel/layer on the fly, rather than requiring it to already
  exist.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a survey of which knob types need the "new channel"
  affordance.

  Acceptance sketch:
  - Channel/layer output selectors offer a "new..." option that creates and
    selects a new channel/layer.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M37)
- New milestone. Suggested file: `PLAN/MILESTONES/M37-channel-management-nodes.md`.
  Board row: `| M37 | Channel/layer management nodes | todo | ... |`

  ```
  # Milestone 37: Channel/layer management nodes

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Add nodes for adding/removing channels/layers, with wildcard and/or regex
  support for selecting which channels/layers to affect.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18).

  Acceptance sketch:
  - A node exists to add channels/layers.
  - A node exists to remove channels/layers, selectable via wildcard or
    regex pattern.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M38)
- New milestone. Suggested file:
  `PLAN/MILESTONES/M38-channel-layer-ui-organization.md`. Board row:
  `| M38 | Improve channel/layer information organization in the node UI | todo | ... |`

  ```
  # Milestone 38: Improve channel/layer information organization in the node UI

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  How nodes organize and display channel/layer information is awkward today.
  Nuke's approach is a reference point, not a model to copy outright. Redesign
  the layout for clarity.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); benefits from M39's terminology decision (layers vs. planes)
  landing first.

  Acceptance sketch:
  - Channel/layer selection UI is redesigned and demonstrably clearer than
    the current layout.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M39)
- New milestone. Suggested file: `PLAN/MILESTONES/M39-layers-not-planes.md`.
  Board row: `| M39 | Adopt "layer" terminology instead of "planes" | todo | ... |`

  ```
  # Milestone 39: Adopt "layer" terminology instead of "planes"

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Replace the app's "planes" terminology with the more standard "layers" (per
  common compositing/EXR usage — confirm exact EXR nomenclature during
  elaboration) across UI strings, docs, and, where safe, internal naming.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a scoping pass on how far the rename reaches (UI-only
  vs. internal APIs/serialization) before elaboration.

  Acceptance sketch:
  - User-facing UI uses "layer" instead of "plane" consistently.
  - Project files and serialization compatibility are unaffected.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M40)
- New milestone. Suggested file: `PLAN/MILESTONES/M40-node-text-wrap.md`.
  Board row: `| M40 | Node text layout: grow to max width, then wrap vertically | todo | ... |`

  ```
  # Milestone 40: Node text layout — grow to max width, then wrap vertically

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Node labels currently can hang off the node's sides. Nodes should grow
  horizontally up to a maximum width, then wrap text onto additional lines
  (grow vertically) instead of overflowing.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18).

  Acceptance sketch:
  - A long node name wraps onto multiple lines once the node reaches its max
    width, instead of overflowing the node's silhouette.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M41)
- New milestone. Suggested file: `PLAN/MILESTONES/M41-icon-replacement-pass.md`.
  Board row: `| M41 | Icon replacement pass | todo | ... |`

  ```
  # Milestone 41: Icon replacement pass

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Many of the app's icons are hard to read. Do a thorough pass
  replacing/redesigning icons for legibility.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs an icon audit and a design direction before elaboration.

  Acceptance sketch:
  - A defined set of hard-to-read icons is identified and replaced with
    clearer versions.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M42)
- New milestone. Suggested file: `PLAN/MILESTONES/M42-stylesheet-overhaul.md`.
  Board row: `| M42 | Stylesheet / look-and-feel overhaul | todo | ... |`

  ```
  # Milestone 42: Stylesheet / look-and-feel overhaul

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  The general stylesheet and layout feel cramped and cluttered. Revisit
  spacing, density, and visual hierarchy across the app.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a design direction/spec before elaboration.

  Acceptance sketch:
  - Key panels (node graph, properties, viewer) read as less cramped, judged
    against before/after screenshots.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M43)
- New milestone. Suggested file: `PLAN/MILESTONES/M43-drop-premult-concept.md`.
  Board row: `| M43 | Drop the premultiplied/unpremultiplied concept | todo | ... |`

  ```
  # Milestone 43: Drop the premultiplied/unpremultiplied concept

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Remove the app's built-in premultiplied/unpremultiplied tracking and
  handling; treat that as the user's responsibility to manage/track, as in
  other professional compositing software.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a scoping pass on everywhere premult state is read or
  written before elaboration, since it likely touches many nodes' metadata
  handling.

  Acceptance sketch:
  - The app no longer tracks or exposes a premultiplied/unpremultiplied
    concept; existing projects still load sensibly.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: M24 (new milestone M44)
- New milestone. Suggested file: `PLAN/MILESTONES/M44-trackball-colour-editing.md`.
  Board row: `| M44 | Trackball-style colour editing | todo | ... |`

  ```
  # Milestone 44: Trackball-style colour editing

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Add a "trackball" interaction to colour controls — holding modifier keys
  and dragging adjusts hue/saturation/value/temperature directly on the
  swatch, rather than only via sliders/dialogs.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); pairs naturally with M24's colour-panel work landing first.

  Acceptance sketch:
  - Dragging on a colour control with the documented modifier held adjusts
    hue, saturation, value, or temperature respectively.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M45)
- New milestone. Suggested file: `PLAN/MILESTONES/M45-tabtabtab-native-tab-menu.md`.
  Board row: `| M45 | Port tabtabtab-nuke as the native tab menu | todo | ... |`

  ```
  # Milestone 45: Port tabtabtab-nuke as the native tab menu

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Port github.com/charlesangus/tabtabtab-nuke as Natron's native tab/create-node
  menu (fuzzy search + smart insertion).

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a look at tabtabtab-nuke's behavior/license and how it
  maps onto Natron's existing node-creation menu before elaboration.

  Acceptance sketch:
  - Pressing the tab-menu shortcut in the node graph opens a fuzzy-searchable
    create-node menu with tabtabtab-style smart insertion onto the selected
    node/pipe.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M46)
- New milestone. Suggested file: `PLAN/MILESTONES/M46-labelmaker-annotations.md`.
  Board row: `| M46 | Port Labelmaker as a native node graph annotation feature | todo | ... |`

  ```
  # Milestone 46: Port Labelmaker as a native node graph annotation feature

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Port github.com/charlesangus/Labelmaker as a standard part of the app, to
  display rich information/annotations on the node graph.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a look at Labelmaker's feature set and license before
  elaboration.

  Acceptance sketch:
  - Nodes can display rich, Labelmaker-style annotation text/data on the node
    graph.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M47)
- New milestone. Suggested file: `PLAN/MILESTONES/M47-render-dialog-rework.md`.
  Board row: `| M47 | Render dialog rework: framerange prompt and foreground-by-default | todo | ... |`

  ```
  # Milestone 47: Render dialog rework — framerange prompt and foreground-by-default

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Render commands should prompt for the desired frame range instead of
  assuming the project range, and renders should happen in the foreground by
  default, with background rendering as an explicit option in the render
  dialog.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18).

  Acceptance sketch:
  - Triggering a render prompts for a frame range (defaulting sensibly, e.g.
    to the project range).
  - Renders run in the foreground unless the user opts into background
    rendering via the dialog.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M48)
- New milestone. Suggested file: `PLAN/MILESTONES/M48-splice-node-into-pipe.md`.
  Board row: `| M48 | Node graph: splice a node/group into an existing pipe by drop | todo | ... |`

  ```
  # Milestone 48: Node graph — splice a node/group into an existing pipe by drop

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Dropping a node or group of nodes onto an existing pipe should pipe it into
  that connection, with a highlight during drag showing how it will be
  spliced in.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18).

  Acceptance sketch:
  - Dragging a node over an existing pipe highlights the pipe to preview the
    splice.
  - Dropping inserts the node between the two previously-connected nodes.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M49)
- New milestone. Suggested file: `PLAN/MILESTONES/M49-roto-feather-multi-select.md`.
  Board row: `| M49 | Roto: feather-handle drag affects all selected points | todo | ... |`

  ```
  # Milestone 49: Roto — feather-handle drag affects all selected points

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  On the Roto node, dragging a feather handle while multiple points are
  selected should adjust the feather for all selected points, not just the
  one being dragged.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18).

  Acceptance sketch:
  - With multiple roto points selected, dragging any one's feather handle
    adjusts feather uniformly (or proportionally, per design) across the
    selection.
  ```

### 2026-09-18T01:31:24-04:00 — change-request
- refs: none (new milestone M50)
- New milestone. Suggested file: `PLAN/MILESTONES/M50-proper-ocio-support.md`.
  Board row: `| M50 | Proper OCIO support as a project property | todo | ... |`

  ```
  # Milestone 50: Proper OCIO support as a project property

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Make OCIO configuration a first-class property of the project, and have the
  Viewer use OCIO for display transforms, rather than today's ad hoc
  handling.

  Blocked on: not yet prioritized — captured as backlog (user request
  2026-09-18); needs a scoping pass on current OCIO usage (M3 already made
  ACES 2.0 Studio the default OCIO config) before elaboration.

  Acceptance sketch:
  - The project has an OCIO config property, persisted with the project.
  - The Viewer's display transform is driven by that OCIO config.
  ```

### 2026-09-18T01:31:32-04:00 — change-request
- refs: none (new milestone M51)
- New milestone. Suggested file: `PLAN/MILESTONES/M51-deep-filtering-opendcx.md`.
  Board row: `| M51 | Deep filtering nodes (OpenDCX integration) | todo | [M51-deep-filtering-opendcx.md](PLAN/MILESTONES/M51-deep-filtering-opendcx.md) |`

  ```
  # Milestone 51: Deep filtering nodes (OpenDCX integration)

  Integrate OpenDCX (DreamWorks' open-source deep compositing extensions
  library) to add production-grade filtering, transformation, and manipulation
  nodes for deep pixel data. Extends M18's core deep compositing foundation
  with filtering operations on multi-sample deep pixels, subpixel masking,
  and efficient deep-data transformations.

  ## Phase 51.1: OpenDCX integration and DeepBlur

  - [ ] M51.P1.T1 — Vendorize OpenDCX and integrate as a CMake dependency
    - files: `tools/cmake/FindOpenDCX.cmake`, `CMakeLists.txt`, build config
    - approach: Add OpenDCX (github.com/dreamworksanimation/opendcx) as a
      pinned vendored dependency (fetch on build). Ensure it links cleanly
      against the existing OpenEXR and Python bindings. Target glibc 2.34+
      compliance on EL9.
    - verify: Build succeeds with OpenDCX statically linked; a minimal test
      that calls OpenDCX's deep API without errors completes.
    - size: M
  - [ ] M51.P1.T2 — Design DeepBlur node: blur on deep pixel data
    - files: `Engine/Nodes/Deep/DeepBlur.cpp`, `.h`, Python bindings
    - approach: Blur (box, Gaussian, or both) applied per-sample on deep
      pixels, preserving sample depth and coverage. Each sample at each pixel
      location is blurred independently. Knobs: blur method (box/Gaussian),
      blur size (XY radii), edge handling (pad/wrap/clamp). Start with a
      single blur method; add others if time/QA headroom permits.
    - verify: GUI test — blur radius knob changes output interactively. CLI
      render: deep blur on a deep image with varying depth/coverage produces
      smooth inter-sample blurring and maintains coverage correctness. Compare
      against OpenDCX reference behavior where applicable.
    - size: L
  - [ ] M51.P1.T3 — Add DeepBlur to the node registry and OFX plugin set
    - files: `HostModel/EffectPluginLoader.cpp`, deep node registry
    - approach: Register DeepBlur as a native node, add category tagging for
      deep nodes, and vendor it in the release AppImage.
    - verify: DeepBlur appears in the Create Node menu under Deep category.
      A project using it loads and renders without errors.
    - size: S

  ## Phase 51.2: DeepTransform (affine transforms on deep pixels)

  - [ ] M51.P2.T1 — Design DeepTransform: scale, rotate, skew deep pixels
    - files: `Engine/Nodes/Deep/DeepTransform.cpp`, `.h`
    - approach: Affine transformation (scale, rotation, skew) applied per-sample
      on deep pixels, preserving sample depth and coverage. Resampling strategy:
      decide between bilinear, bicubic, or OpenDCX's built-in resampling. Knobs:
      translate (XY), scale (XY or uniform), rotate (degrees), and optional
      skew (shear). Reference OpenDCX's DeepTransform class for efficient
      subpixel-accurate implementation.
    - verify: GUI interactive transform. CLI render: a rotated deep image shows
      correct depth ordering and no sample loss at boundaries. Verify subpixel
      accuracy via a test with fractional transforms.
    - size: L
  - [ ] M51.P2.T2 — Ship DeepTransform in the deep node registry
    - files: same registry as M51.P1.T3
    - approach: As M51.P1.T3 — register and vendor.
    - verify: Menu presence, load/render verification as above.
    - size: S

  ## Phase 51.3: Deep utility nodes (QA and Polish)

  - [ ] M51.P3.T1 — Add DeepSamples and DeepInfo inspection nodes
    - files: `Engine/Nodes/Deep/DeepInfo.cpp`, `.h`
    - approach: Read-only nodes that output deep-pixel statistics/visualization.
      DeepInfo outputs depth range, sample count per-pixel, coverage, and
      channel list as readable metadata overlays or log output. No knobs,
      pure inspection.
    - verify: Xvfb GUI: connect DeepInfo to a deep image, viewer shows per-pixel
      statistics correctly. CLI: log output matches expected format.
    - size: M
  - [ ] M51.P3.T2 — Polish deep node UI and docs
    - files: doc strings, Help menu entries, example projects
    - approach: Write brief help text for DeepBlur and DeepTransform. Add
      example .ntp projects demonstrating each node. Ensure knob tooltips
      explain subpixel precision, sample depth semantics, and coverage
      preservation.
    - verify: Help text is searchable and explains each knob. Example projects
      load and render without errors.
    - size: S

  **Verification gate:** DeepBlur and DeepTransform nodes ship, load existing
  deep projects without errors, and render deep images with correct per-sample
  filtering/transformation. Example projects demonstrating both exist. OpenDCX
  library is vendored and statically linked; no external runtime dependency.

  ## Decisions

  - 2026-09-18 — **OpenDCX chosen**: DreamWorks' production-grade deep
    compositing library. Rationale: widely used in film VFX, C++ API integrates
    cleanly with OpenEXR, provides battle-tested filtering and transform
    algorithms on deep pixels. Alternative (DIY implementation) ruled out due to
    algorithmic complexity and risk of sample-loss bugs.
  - 2026-09-18 — **Phase sequencing**: DeepBlur first (simpler, high immediate
    value), DeepTransform second (more complex, useful for deep manipulation
    workflows), then utility/polish. Blocks on M18 shipping; pairs naturally
    with M21 (deep tier-2 nodes).
  ```

### 2026-09-18T02:15:00-04:00 — change-request
- refs: none (new design doc + project-wide decision; governs M52–M54 below)
- Create `PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md` with the
  content below. It amends Part 2 of
  `PLAN/DESIGN/2026-09-05-deep-and-3d-native-extensions.md` per that doc's
  scope-gravity rule ("growth happens in named milestones with this doc
  amended first"). Also create
  `PLAN/DECISIONS/2026-09-18-geometry-is-a-fourth-data-kind.md` and append
  its INDEX line (both given after the design doc). Then add this bullet to
  the board's `# Context and constraints`, after the M32 bullet:

  ```
  - **M52–M54 (3D roadmap) authored 2026-09-18** from
    `PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`, which amends the
    2026-09-05 design doc's Part 2 and governs on any brief's ambiguity.
    Order is fixed by user priority: **M52** (USD layer-stack completion:
    schema-driven edits, path-expression selection, composition nodes) after
    M20's gate → **M53** (projected texture painting: CPU UV bake,
    ProjectTexture, live textures into Hydra) after M52 → **M54** (SOPs-style
    geometry: `eDataKindGeometry`, `GeoDetail`, bridges, operators, point
    editing) after M53. Geometry as a fourth data kind is decided in
    `PLAN/DECISIONS/2026-09-18-geometry-is-a-fourth-data-kind.md`.
  ```

  And add this item under the board's `# Open questions`:

  ```
  - **USD Python inside Natron's Python?** M19.P1.T1 builds USD with Python
    OFF (Shiboken6/PySide6 vs pxr coexistence). A Solaris-style "Python over
    the stage" node would cover the LOPs long tail cheaply but needs pxr
    bindings loaded into Natron's interpreter. M52.P3.T3's `SetAttribute3D`
    is the no-Python substitute for now. Decide whether to spike pxr-in-Natron
    coexistence after M52 ships, or leave it out of scope.
  ```

  Design doc content (`PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`):

  ````
  # Design: 3D roadmap after M20 — LOPs completion, projected texture painting, SOPs-style geometry

  2026-09-18. Amends Part 2 of
  `2026-09-05-deep-and-3d-native-extensions.md` per its scope-gravity rule.
  That doc's decisions stand unchanged: USD is the substrate (model A), a
  `ScenePayload` layer stack flows along scene edges, composition happens
  only at consumers, the viewport is Storm through `UsdImagingGLEngine`, and
  `SceneOps` is a thin seam, not an abstraction over USD. This doc answers
  three questions the user raised after that doc was written and turns the
  answers into milestones M52, M53 and M54, in that order.

  ## Question 1 — can the USD node surface be produced mechanically?

  Mostly yes, and M19/M20 already built the parts that can't be. USD's data
  model is uniform (prims, attributes, relationships, layers, composition
  arcs, schemas), so under the layer-stack model most nodes collapse to a few
  generic ones:

  - **Schema-driven edit/create nodes.** Iterate `UsdSchemaRegistry`,
    generate knobs from each attribute's type, default and doc. One generic
    node parameterized by schema type covers every typed schema and every
    applied API schema. Houdini's *Edit Properties* / *Create Primitive*
    LOPs are literally this.
  - **Composition nodes.** Reference/Payload, Variant select, Kind/Purpose,
    Activate/Visibility (M20's `Prune3D`). Each is one Sdf call over a prim
    selection.
  - **Material assign, collections, Write.** Already M20.
  - **Render.** One Hydra node parameterized by delegate — M20's
    `HydraRender`.

  What is *not* mechanical is the plumbing, and M19/M20 own it: the scene
  data kind and its hashing/cache (M19.P1), the viewport (M19.P2), time
  policy (M19.P1.T3), and the render bridge (M20.P3.T1). Three pieces are
  still missing and are what M52 adds:

  1. **Authored-only semantics.** A generated node with forty knobs must
     write opinions only for knobs the user touched, or every node stomps
     every attribute with defaults. Solaris solves this with a per-parameter
     enable toggle. This needs knob-level support (`AuthoredKnobSet`).
  2. **Knob-type mapping.** Tokens with allowed values, matrices, asset
     paths, arrays and time-sampled arrays have no clean knob equivalent.
     M52 ships a mapping table (`SchemaKnobMapper`) and explicitly skips
     array-valued attributes; those are geometry data and belong to M54.
  3. **Prim selection.** Use `SdfPathExpression` (USD ≥ 23.11) evaluated
     over the composed input stage, rather than inventing a pattern
     language. One `PrimSelector` knob helper replaces M20's "path or
     wildcard" per-node parsing.

  A Solaris-style Python-over-the-stage node is the cheapest way to cover
  the long tail but conflicts with M19.P1.T1's Python-OFF USD build. It is
  an open question on the board, not part of M52. `SetAttribute3D` (author
  one named attribute of a chosen type on a selection) is the no-Python
  escape hatch.

  Generated schema nodes feel clunky to artists (Katana and Solaris users
  both say so). M20's hand-curated `Camera3D`/`Light3D`/`Transform3D`/
  `Material3D` stay the primary UI; `EditPrim3D` is the baseline that makes
  every other attribute reachable.

  ## Question 2 — does geometry editing / texturing fit a USD assembly model?

  The mismatch is not with USD, which has a full authoring API and stores
  meshes as plain point/index arrays. The mismatch is with the *layer-stack
  node graph convention*: edits are sparse opinions, while modeling and
  painting produce dense bulk data and destructive edit histories. Houdini
  split SOPs from LOPs for exactly this reason; the same split is adopted
  here without building a modeling package.

  ### Texture painting fits well, because it is a 2D problem

  Mari's architecture is projection painting: paint in screen space, bake to
  UV space. Natron already has RotoPaint and a strong 2D pipeline, so M53
  adds only the 3D-specific pieces:

  - **`SceneRaster`** — a small CPU triangle rasterizer over a composed
    stage (triangulation via Hydra's `HdMeshUtil`, which USD ships). Two
    modes: camera-space z-buffer (depth, prim id, world position per pixel)
    and UV-space bake (world position and normal per texel of a prim's `st`
    primvar). CPU, GL-free, so it runs in CI under Xvfb.
  - **`ProjectTexture`** (scene + image → image) — for every texel of the
    target prim's UV space, take the world point from the UV bake, project
    it through the chosen camera, occlusion-test against the camera z-buffer,
    sample the input image. Output is a normal Natron `Image`, so it goes
    through the image cache and every 2D node downstream.
  - **Live textures** — Hydra loads textures through the `Ar` resolver and
    `Hio` image plugins. A `natron://<node>/<plane>?v=<hash>` scheme served
    by an in-process `ArResolver` + `HioImage` plugin lets `Material3D` bind
    a *node output* as a texture with no disk round trip. The `?v=<hash>`
    query makes a changed image a new texture path, which is the simplest
    reliable way to make Storm reload. `WriteScene` exports live textures
    to files beside the USD so exported scenes stay portable.
  - **Paint workflow** = `HydraRender` (through camera) → `RotoPaint` →
    `ProjectTexture` (same camera) → 2D `Merge` over the existing texture →
    `Material3D` → `Viewport3D`. Every brush feature is the existing 2D
    RotoPaint. Multi-camera layering is several `ProjectTexture`s and a 2D
    merge. A shipped preset group wires this up as one node.

  Storm's `UsdPreviewSurface` has no camera-projection mode, so projection
  is a *bake* (`ProjectTexture`), never a live shader. That is also what
  Nuke's Project3D and Mari do at export time.

  ### Geometry: what fits and what doesn't

  Fits cleanly (M54):
  - **Generators** (card, cube, sphere, cylinder, grid, OBJ import) and
    **whole-array deformers** (transform, displace, subdivide, normals, UV
    project). `VtArray` is copy-on-write, so passing a million-point array
    between nodes costs a pointer.
  - **EditGeo-style point tweaks**: a sparse `point index → offset` map
    stored as knob data, applied at cook time. Nuke's classic EditGeo.
    Known caveat, same as Nuke: edits break if upstream topology changes;
    the node detects a count mismatch and warns rather than misapplying.
  - **Groups and attribute-driven selection**, which is the thing that makes
    SOP work powerful and has no home in the layer-stack model.

  Does not fit and is out of scope: topology-changing interactive tools
  (extrude, bevel, loop cut, booleans, remesh, retopo). Each is a
  destructive operation with an edit history; in a non-destructive graph
  that is a node per click or an opaque blob node. "Basic modeling" is
  defined as generators + deformers + point tweak + transform gizmos.

  ## Question 3 — what does a SOP/LOP split look like here?

  A **fourth data kind**, `eDataKindGeometry`, and a `GeoDetail` container:

  ```
  GeoDetail (immutable value, COW parts)
  ├─ prims : vector<GeoPrim>
  │    kind      : mesh | points | curves
  │    path      : TfToken                  // becomes the prim path at the bridge
  │    points, faceVertexCounts, faceVertexIndices : VtArray
  │    attrs     : map<(interpolation, name), VtValue>   // UsdGeom primvar model
  │    groups    : map<name, bitset>        // point or prim groups
  ├─ contentHash : U64
  └─ getSizeInBytes()                       // aliasing-blind, as DeepImage
  ```

  It is deliberately UsdGeom's attribute set without a stage, built from USD
  types (`VtArray`, `TfToken`, `VtValue`, `Gf*`) behind a pimpl so no pxr
  header reaches `Engine/` core — the same rule as `ScenePayload`. Because
  the container *is* UsdGeom's vocabulary, the bridge is a lookup table:

  - `GeoToScene` authors a detail as prims under a scope path into the
    node's layer, at the current frame by default (option: time samples
    across the range per M19's policy — off by default, since vertex data
    is exactly what that policy says never to bake automatically).
  - `SceneToGeo` extracts prims matching a `PrimSelector` expression into a
    detail at the current time, optionally flattening xforms.

  What the split buys over "geometry nodes are Stage nodes authoring dense
  overrides under a scratch scope":

  - Geo nodes mutate arrays directly with COW. No layer per node, no
    recomposition; `GeoDelete` is a filtered copy rather than a
    deactivation plus a rewritten mesh.
  - Per-frame semantics like `Image`, so no time-sample policy inside the
    geo chain. Time is a question only at the bridge.
  - Groups exist.

  What it shares with the scene side, at zero extra cost:

  - **Viewport.** Viewing a geometry node wraps it through `GeoToScene` into
    a scratch stage (cached in `StageCache` by detail hash) and renders
    through the same `Viewport3D`. Picking returns prim path + point index,
    which maps back to the detail.
  - **Cache and scheduling.** `Cache<GeoDetail>` is the third instantiation
    of the templated cache after `Image` and `DeepImage`, with its own
    budget knob; `renderGeometry()` mirrors M18's `renderDeep()` pull path.
    The deep milestone already paid for the dispatch points.
  - **Picking and gizmos.** M20.P2.T1's translate gizmo is reused for point
    drags; M54 adds a point-picking mode to `Viewport3D`.

  A `GeoWrangle` node exposes detail arrays to Natron's *own* Python via the
  buffer protocol (`memoryview`; numpy optional through `np.frombuffer`).
  That is Natron Python plus a tiny CPython extension type — it needs no
  pxr Python bindings and does not touch the open question above.

  ## Effort on top of the USD milestones

  | Piece | Size | Where |
  |---|---|---|
  | Authored-only knobs, schema→knob table, path expressions | Medium | M52 |
  | Composition nodes | Small | M52 |
  | CPU raster substrate, ProjectTexture | Medium | M53 |
  | Live-texture resolver + Hydra reload | Medium | M53 |
  | GeoDetail container, cache, renderGeometry pull path | Medium | M54.P1 |
  | Bridges, scratch-stage viewing | Small | M54.P2 |
  | First operator set (≈10 nodes) + wrangle | Medium | M54.P3 |
  | Point picking + GeoEdit | Medium | M54.P4 |
  | Booleans, remesh, retopo, interactive topology tools | Large | out of scope |

  Roughly a third to a half again on top of M19+M20 for the whole of
  M52–M54; M54's operator library then grows linearly per node.

  ## Sequencing and dependencies

  - **M52** requires M20's gate (`SceneOps`, `Prim3D`, `Transform3D`,
    `Prune3D`, `Material3D`, `WriteScene`, the M20.P3.T3 integration test it
    extends).
  - **M53** requires M52 (`PrimSelector`) and M20 (`HydraRender`,
    `Material3D`, `Viewport3D`).
  - **M54** requires M52 (`PrimSelector`) and M20.P2.T1 (gizmo). It does not
    technically need M53; it runs after M53 by user priority.

  ## Node vocabulary added by this amendment (the new cap)

  - **M52**: `EditPrim3D`, `Prim3D` (gains schema type), `Reference3D`,
    `Variant3D`, `SetAttribute3D`.
  - **M53**: `ProjectTexture`, `Material3D` (gains image inputs),
    `WriteScene` (gains texture export), preset group `PaintThroughCamera`.
  - **M54**: `GeoToScene`, `SceneToGeo`, `GeoPrimitive`, `GeoImportOBJ`,
    `GeoTransform`, `GeoMerge`, `GeoGroup`, `GeoDelete`, `GeoDisplace`,
    `GeoNormals`, `GeoSubdivide`, `GeoUVProject`, `GeoWrangle`, `GeoEdit`.

  As before: these are caps, not floors. Further nodes need a named
  milestone and an amendment here first.

  ## Risks

  - **Schema knob explosion** — `EditPrim3D` on a large schema builds many
    knobs; build them lazily per page and skip arrays. Watch property-panel
    performance with `UsdGeomMesh` selected.
  - **Live-texture lifetime** — the `Ar` registry holds weak node refs and
    must tolerate a node deleted while Storm still references its path;
    resolve to a missing-asset error, never a dangling pointer.
  - **Hydra texture reload** — the `?v=<hash>` path trick is simple but
    means Storm re-uploads the whole texture on every stroke. Acceptable
    for v1; `HdStDynamicUvTextureObject` is the upgrade path if paint
    latency is unacceptable.
  - **GeoEdit topology drift** — sparse edits keyed by point index. Detect
    and warn; do not attempt to remap.
  - **Scope gravity, again** — M54's operator list is the SOP cap.
  ````

  Decision file content
  (`PLAN/DECISIONS/2026-09-18-geometry-is-a-fourth-data-kind.md`):

  ```
  # Geometry is a fourth data kind, split SOP/LOP-style from the scene stack

  2026-09-18. Editable geometry flows through the graph as a fourth
  `DataKindEnum` value, `eDataKindGeometry`, carrying an immutable
  copy-on-write `GeoDetail` (UsdGeom's attribute vocabulary without a
  stage), rather than as dense attribute overrides authored into the
  `ScenePayload` layer stack. Bridge nodes (`GeoToScene`, `SceneToGeo`)
  convert at explicit points, exactly as `DeepToImage`/`DeepFromImage` do
  for deep. Rationale: modeling-style operators mutate whole arrays, want
  per-frame `Image`-like semantics, and need groups — none of which the
  layer-stack convention provides — while the container built from
  `VtArray`/`TfToken`/`VtValue` keeps the bridge a lookup table and lets
  the same `Viewport3D`/Storm path display geometry through a scratch
  stage. The design is `PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
  Question 3; the work is M54.
  ```

  INDEX line to append to `PLAN/DECISIONS/INDEX.md`:

  ```
  - 2026-09-18 — geometry-is-a-fourth-data-kind: editable geometry is `eDataKindGeometry` + COW `GeoDetail`, bridged explicitly to/from the scene stack (SOP/LOP split); work is M54 → DECISIONS/2026-09-18-geometry-is-a-fourth-data-kind.md
  ```

### 2026-09-18T02:15:00-04:00 — change-request
- refs: none (new milestone M52; requires M20's gate)
- New milestone. File: `PLAN/MILESTONES/M52-usd-layer-stack-completion.md`.
  Board row, placed after M21's row:
  `| M52 | USD layer-stack completion: schema-driven edits, path expressions, composition nodes | todo | [M52-usd-layer-stack-completion.md](PLAN/MILESTONES/M52-usd-layer-stack-completion.md) |`

  ```
  # Milestone 52: USD layer-stack completion — schema-driven edits, path expressions, composition nodes

  Stage 1 of the 3D roadmap (`PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
  Question 1). M19/M20 built the scene substrate and a hand-curated node
  set; this milestone adds the three pieces that make the rest of USD's
  surface reachable without a node per attribute: `SdfPathExpression` prim
  selection shared by every pattern knob, authored-only knob semantics, and
  a schema-driven `EditPrim3D`. Then the small set of composition nodes
  that are one Sdf call each. Requires M20's gate. All nodes are
  `NativeEffectBase` subclasses in `Engine/Nodes/Scene/`, one anonymous
  layer each via `SceneOps`, memoized per M19.P1.T3, registered in
  `AppManager::loadBuiltinNodePlugins()` under `PLUGIN_GROUP_3D`.

  ## Phase 52.1: Selection and authored-only substrate

  - [ ] M52.P1.T1 — `PrimSelector`: one path-expression knob helper for every scene node
    - files: `Engine/Nodes/Scene/PrimSelector.h`/`.cpp`, `Engine/Nodes/Scene/SceneOps.h`/`.cpp`, `Tests/PrimSelector_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: a `KnobString` wrapper whose value is an `SdfPathExpression` (USD ≥ 23.11), plus `SceneOps::selectPrims(composedStage, expression, time) → vector<SdfPath>` evaluated over a stage traversal. Supports the expression language as shipped (`/World/geo//*`, `//*Light*`, set operators); an invalid expression sets a persistent node error naming the parse failure rather than matching nothing silently. Pxr types stay behind the pimpl.
    - verify: unit tests against the M19.P2.T4 reference `.usda` — absolute path, wildcard, descendant and union expressions each return the expected prim set; an invalid expression reports an error string.
    - size: M

  - [ ] M52.P1.T2 — Migrate M20's pattern knobs onto `PrimSelector`
    - files: `Engine/Nodes/Scene/Transform3D.cpp`, `Engine/Nodes/Scene/Prune3D.cpp`, `Engine/Nodes/Scene/Material3D.cpp`, `Tests/` (the M20.P3.T3 scene vocabulary test)
    - approach: replace each node's "path or wildcard" knob with `PrimSelector`; knob script-name unchanged so M20-era projects load. Behaviour for the old plain-path and `*` forms must be identical (they are valid `SdfPathExpression`s).
    - verify: M20.P3.T3 integration test still green unchanged; a project saved with M20's Transform3D loads and composes the same override.
    - size: S

  - [ ] M52.P1.T3 — `AuthoredKnobSet`: only knobs the user touched write opinions
    - files: `Engine/Nodes/Scene/AuthoredKnobSet.h`/`.cpp`, `Tests/AuthoredKnobSet_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: helper owning a group of value knobs, each paired with a hidden-by-default `KnobBool` "author" companion (Solaris' per-parameter enable). The companion flips true on the first user edit of its value knob (hook `onKnobValueChanged` with a user-edit reason), is shown as a small toggle beside the knob, and is serialized like any knob. `forEachAuthored(fn)` visits only enabled pairs; resetting a value knob to default clears its companion. Animated knobs with the companion set author time samples per M19.P1.T3.
    - verify: unit test — a set with three knobs where one is edited reports exactly one authored knob; the state round-trips through project serialization; a reset-to-default clears it.
    - size: M

  ## Phase 52.2: Schema-driven edit node

  - [ ] M52.P2.T1 — `SchemaKnobMapper`: schema attribute definitions → knob descriptions
    - files: `Engine/Nodes/Scene/SchemaKnobMapper.h`/`.cpp`, `Tests/SchemaKnobMapper_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: for a typed or applied-API schema name, walk `UsdSchemaRegistry`'s prim definition and emit a list of `(attrName, knobType, dimension, default, allowedTokens, doc)`. Mapping table: bool→`KnobBool`; int/uint→`KnobInt`; float/double/half→`KnobDouble`; float2/3/4, double2/3/4→`KnobDouble` dims 2–4; color3f/color4f→`KnobColor`; token with `allowedTokens`→`KnobChoice`, token without→`KnobString`; string→`KnobString`; asset→`KnobFile`; matrix/quat/array-valued attributes and relationships are **skipped** and returned in a separate "unsupported" list (arrays are geometry data — M54). Also lists the schema's typed and applied-API names for a chooser.
    - verify: unit test — `UsdGeomCamera` maps to knobs for focalLength, horizontalAperture, clippingRange (2-dim) and projection (choice with `perspective`/`orthographic`); `UsdGeomMesh` reports `points`/`faceVertexIndices` in the unsupported list.
    - size: M

  - [ ] M52.P2.T2 — `EditPrim3D` node
    - files: `Engine/Nodes/Scene/EditPrim3D.h`/`.cpp`, `Engine/AppManager.cpp` (registration), `Tests/EditPrim3D_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: inputs: Scene. Knobs: `PrimSelector`, a schema chooser (`KnobChoice` from M52.P2.T1's list, typed schemas and applied APIs), then one `KnobPage` of knobs built by `SchemaKnobMapper` inside an `AuthoredKnobSet`, rebuilt when the schema knob changes (knobs for the previous schema are removed; serialized values for the current schema are restored). Render: for each selected prim, author exactly the authored knobs' values as attribute opinions in the node's layer; an applied-API schema also authors the `apiSchemas` listOp. Never creates prims (that is `Prim3D`). Skipped attributes are listed in the node's help text.
    - verify: unit test — over the reference camera prim, editing only focalLength yields an exported layer whose only opinion under that prim is `focalLength`; switching schema to `CollectionAPI` and setting one field authors the apiSchemas entry plus that field. Manual: property panel with `UsdGeomMesh` selected stays responsive.
    - size: L

  - [ ] M52.P2.T3 — `Prim3D` gains a schema type
    - files: `Engine/Nodes/Scene/Prim3D.cpp`, `Tests/` (M20's Prim3D test)
    - approach: replace M20's "empty xform / simple prim" choice with the `SchemaKnobMapper` typed-schema list; the created prim gets that type, and its attributes are exposed through an `AuthoredKnobSet` exactly as in `EditPrim3D` (share the page-building code — factor it into `SchemaKnobMapper` or a small `SchemaKnobPage` helper if T2 didn't already).
    - verify: unit test — `Prim3D` with type `SphereLight` authors a prim of that type; with `Xform` (the default) behaves exactly as M20's test expects.
    - size: S

  ## Phase 52.3: Composition nodes

  - [ ] M52.P3.T1 — `Reference3D`
    - files: `Engine/Nodes/Scene/Reference3D.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/Reference3D_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: inputs: Scene (target stack), optional Scene (source stack). Knobs: target prim path, mode `reference | payload`, source = file path *or* the source input's composed default prim (the input stack's strongest layer is referenced by its anonymous identifier, which is valid in-process), `instanceable` bool. Authors the reference/payload listOp on the target prim in the node's layer. Payloads are loaded by default at consumers.
    - verify: unit test — referencing the reference `.usda` under `/World/ref` composes its prims there; referencing a second `ReadScene` input does the same with no file involved; `instanceable` shows on the composed prim.
    - size: M

  - [ ] M52.P3.T2 — `Variant3D`
    - files: `Engine/Nodes/Scene/Variant3D.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/Variant3D_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: inputs: Scene. Knobs: `PrimSelector`, variant set (`KnobChoice` populated from the composed input's variant sets on the first selected prim, plus free text), variant name (same, from that set's variants). Authors the variant selection on each selected prim. Populating the choices reads the composed input stage through `StageCache`; a selection naming a set the prim lacks is authored anyway (USD ignores it) but flagged in the node's info message.
    - verify: unit test — a hand-authored `.usda` with a `shadingVariant` set: selecting `red` vs `blue` changes the composed material binding; choice knobs list both variants.
    - size: M

  - [ ] M52.P3.T3 — `SetAttribute3D`
    - files: `Engine/Nodes/Scene/SetAttribute3D.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/SetAttribute3D_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: inputs: Scene. Knobs: `PrimSelector`, attribute name, value type (`KnobChoice` over the scalar/vector types `SchemaKnobMapper` supports, plus `token`), value knob rebuilt per type, `primvar` bool (authors `primvars:<name>` with `constant` interpolation), animated → time samples. The no-Python escape hatch for anything `EditPrim3D` can't reach: custom attributes, primvars, arbitrary metadata-free opinions.
    - verify: unit test — authoring `primvars:displayColor` as color3f on the reference mesh composes a constant primvar; a custom `float` attribute round-trips through `WriteScene`.
    - size: S

  - [ ] M52.P3.T4 — Extend the scene vocabulary integration test
    - files: `Tests/` (M20.P3.T3's test), a second small reference `.usda` with a variant set
    - approach: extend the headless graph with `Reference3D` → `Variant3D` → `EditPrim3D` → `SetAttribute3D` → `WriteScene`, asserting the exported composition: referenced prims present, selected variant applied, only the authored attribute opinions present, custom attribute present.
    - verify: test green in `build-and-test`.
    - size: S

  **Verification gate:** CI green including M52.P3.T4; `EditPrim3D` over a camera prim exports exactly the touched attributes and nothing else; `Variant3D` switches a variant visible in `Viewport3D` on real hardware; every M20 pattern knob is a `PrimSelector` and M20-era projects still load; pre-existing ctest suite green.
  ```

### 2026-09-18T02:15:00-04:00 — change-request
- refs: none (new milestone M53; requires M52's gate)
- New milestone. File: `PLAN/MILESTONES/M53-projected-texture-painting.md`.
  Board row, placed after M52's row:
  `| M53 | Projected texture painting: UV bake, ProjectTexture, live textures | todo | [M53-projected-texture-painting.md](PLAN/MILESTONES/M53-projected-texture-painting.md) |`

  ```
  # Milestone 53: Projected texture painting — UV bake, ProjectTexture, live textures

  Stage 2 of the 3D roadmap (`PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
  Question 2, "Texture painting"). Mari-style projection painting built from
  Natron's existing 2D tools: a CPU rasterizer that bakes a prim's UV space
  to world positions, a `ProjectTexture` node that projects any image
  through a camera into that UV space with occlusion, and an in-process
  `Ar`/`Hio` plugin so `Material3D` can bind a node's output as a live
  texture in Storm. Requires M52 (`PrimSelector`) and M20 (`HydraRender`,
  `Material3D`, `Viewport3D`, `WriteScene`).

  ## Phase 53.1: CPU raster substrate

  - [ ] M53.P1.T1 — `SceneRaster`: camera-space z-buffer and UV-space bake over a composed stage
    - files: `Engine/Nodes/Scene/SceneRaster.h`/`.cpp`, `Tests/SceneRaster_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: pure-CPU triangle rasterizer, no GL. Input: composed `UsdStage` (via `StageCache`), `UsdTimeCode`, prim set. Triangulate `UsdGeomMesh` faces with Hydra's `HdMeshUtil` (ships in the USD lib; handles n-gons and primvar index remapping). Mode A, camera: given a `UsdGeomCamera` path and a resolution, write depth, prim id and world position per pixel with a z-test. Mode B, UV bake: given one prim, its `st` primvar name and a resolution, rasterize triangles in UV space and write world position and world normal per texel, plus a coverage mask. Parallel over scanline chunks (reuse `NativeEffectBase::makeDeepScanlineChunks`' partition shape). Subdivision surfaces rasterize their control cage (documented limitation).
    - verify: unit tests on hand-authored `.usda` — a unit quad facing the camera fills the expected pixel rect with monotonic depth; a UV-mapped quad bakes a full 0–1 coverage mask whose corner texels carry the quad's corner world positions.
    - size: L

  - [ ] M53.P1.T2 — Primvar interpolation and UDIM tile selection in the UV bake
    - files: `Engine/Nodes/Scene/SceneRaster.cpp`, `Tests/SceneRaster_Test.cpp`
    - approach: handle `faceVarying`, `vertex` and `varying` `st` (indexed or not) through `HdMeshUtil`'s primvar remap; add a UDIM tile parameter so the bake covers tile `1001 + u + 10*v` — UV values are offset by the tile origin before rasterization. Missing `st` sets a node error naming the prim.
    - verify: unit test — the same quad with `faceVarying` indexed `st` bakes identically to the `vertex` case; a quad whose UVs lie in tile 1002 bakes empty at 1001 and full at 1002.
    - size: M

  ## Phase 53.2: Nodes

  - [ ] M53.P2.T1 — `ProjectTexture` node: scene + image → UV-space image
    - files: `Engine/Nodes/Scene/ProjectTexture.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/ProjectTexture_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: inputs: Scene, Image (the thing to project; typically a `HydraRender` + paint, or any image). Knobs: camera path, `PrimSelector` (first match is the bake target), `st` primvar name, UDIM tile, output resolution, occlusion on/off with a depth bias, edge dilation in texels. Render (output kind **image**, RoD = resolution): UV-bake the prim with `SceneRaster`; for each covered texel project its world position through the camera into the input image's pixel space and sample bilinearly; texels occluded per the camera z-buffer, off-screen, or back-facing get alpha 0; dilate the result by N texels so bilinear texture filtering doesn't bleed black. The input image is fetched for the camera's full frame at the project format. Result is a normal `Image` and caches normally.
    - verify: unit test — a constant red image projected onto a camera-facing UV quad yields an all-red, alpha-1 texture inside the coverage mask; a second quad in front of half of it leaves that half at alpha 0 with occlusion on and red with it off.
    - size: L

  - [ ] M53.P2.T2 — Live-texture registry and `Ar` resolver plugin
    - files: `Engine/Nodes/Scene/LiveTextureRegistry.h`/`.cpp`, `Engine/Nodes/Scene/LiveTextureResolver.h`/`.cpp` (`ArResolver` + `ArAsset`), `Engine/Nodes/Scene/plugInfo.json` + CMake to install it where `PlugRegistry` finds it, `Tests/LiveTexture_Test.cpp`
    - approach: URI scheme `natron://<nodeScriptName>/<plane>?v=<imageHash>`. The registry maps node script name → weak `NodePtr` plus the time the image was registered for (set by `Material3D` at layer-authoring time). The resolver answers `Resolve()` with the URI itself when the node exists and `OpenAsset()` with an in-memory `ArAsset` wrapping the node's rendered RGBA float image for that time. A URI whose node is gone resolves to nothing, so Hydra shows the missing-texture fallback rather than crashing. Registered via `plugInfo.json`; USD's plugin path must include the Natron binary's plugin dir in the installed and AppImage layouts.
    - verify: unit test — register a `Constant` node, `ArGetResolver().OpenAsset()` on its URI returns pixel bytes of the expected size; unregistering makes the resolve fail cleanly. Packaging (M15) still builds with the plugin dir.
    - size: L

  - [ ] M53.P2.T3 — `Hio` image plugin for live-texture assets
    - files: `Engine/Nodes/Scene/LiveTextureImage.h`/`.cpp` (`HioImage` subclass), `Engine/Nodes/Scene/plugInfo.json`, `Tests/LiveTexture_Test.cpp`
    - approach: an `HioImage` implementation for the `natron` scheme that reads the `ArAsset` from T2 as RGBA float with the correct size and orientation (Natron images are bottom-up; Hio expects top-down — flip once). Registered in the same `plugInfo.json`. This is what lets Storm's texture loader consume a node output without touching disk.
    - verify: unit test — `HioImage::OpenForReading()` on a registered `Constant` URI reads back matching dimensions and the constant's colour at the corners.
    - size: M

  - [ ] M53.P2.T4 — `Material3D` texture inputs bound as live textures
    - files: `Engine/Nodes/Scene/Material3D.cpp`, `Engine/Nodes/Scene/WriteScene.cpp`, `Tests/` (Material3D and WriteScene tests)
    - approach: add optional **Image** inputs to `Material3D` for diffuseColor, roughness, metallic, normal and emissive alongside the existing file-path knobs (a connected input wins). For a connected input, author a `UsdUVTexture` shader whose `file` is the `natron://` URI with `?v=<input image hash>`, register the node with `LiveTextureRegistry`, and fold the input image hash into the node's layer hash so a changed texture yields a new layer, a new stage and a new texture path Storm reloads. `WriteScene` gains "export live textures": on export, each `natron://` reference is rendered to `<usd basename>_textures/<node>_<plane>.exr` next to the output and the exported layer's path is rewritten to it.
    - verify: unit test — a `Constant` → `Material3D` diffuse input composes a `UsdUVTexture` with a `natron://` asset path; `WriteScene` produces the `.exr` and a layer referencing the relative file path that `ReadScene` composes cleanly.
    - size: M

  - [ ] M53.P2.T5 — `Viewport3D` refreshes when a live texture's source changes
    - files: `Gui/Viewport3D.cpp`, `Engine/Nodes/Scene/Material3D.cpp`
    - approach: the viewport already re-composes when the viewed node's stack hash changes (M19.P1.T3); confirm the `?v=<hash>` path change propagates through Storm's texture registry as a reload, and add an explicit `HdChangeTracker` material-dirty mark if a stale texture persists. Keep it simple: whole-texture reload per change is acceptable for v1 (design doc, Risks).
    - verify: manual — paint a stroke upstream of a live-textured material and see it appear on the model in `Viewport3D` without touching the viewport.
    - size: S

  ## Phase 53.3: Paint workflow

  - [ ] M53.P3.T1 — `PaintThroughCamera` preset group
    - files: a shipped PyPlug/preset under the existing built-in presets location, `Gui/` only if the preset loader needs a menu entry, brief usage notes in `Engine/Nodes/Scene/README.md` (create if absent)
    - approach: a group node wiring `HydraRender`(camera) → `RotoPaint` → `ProjectTexture`(same camera, same prim) → 2D `Merge` over an optional existing-texture input → output image, with the camera path and prim selector promoted to the group. Its output feeds `Material3D`'s diffuse input. Multi-angle painting is several of these merged in 2D; say so in the notes.
    - verify: manual on real hardware — paint a stroke in the 2D viewer on the rendered view; the stroke shows on the model in `Viewport3D` from a different angle, correctly occluded where another prim was in front.
    - size: M

  - [ ] M53.P3.T2 — Paint pipeline integration test in CI
    - files: `Tests/ProjectTexture_Test.cpp` (extend), reference `.usda` with a UV-mapped quad and an occluder
    - approach: headless graph — `ReadScene` + `Camera3D` + `Constant` → `ProjectTexture` → assert coverage and occlusion; `ProjectTexture` → `Material3D`(live) → `WriteScene`(export textures) → assert the exported `.exr` matches the `ProjectTexture` output. No GL involved (`SceneRaster` is CPU).
    - verify: test green in `build-and-test`.
    - size: S

  **Verification gate:** CI green including M53.P3.T2; on real hardware the `PaintThroughCamera` round trip works end to end with occlusion; `WriteScene` exports a portable USD + textures that `usdview` displays with the painted texture; packaging still builds with the `Ar`/`Hio` plugin discoverable; pre-existing ctest suite green.
  ```

### 2026-09-18T02:15:00-04:00 — change-request
- refs: none (new milestone M54; requires M52's gate and M20.P2.T1; sequenced after M53 by user priority)
- New milestone. File: `PLAN/MILESTONES/M54-sops-style-geometry.md`.
  Board row, placed after M53's row:
  `| M54 | SOPs-style geometry: GeoDetail data kind, bridges, operators, point editing | todo | [M54-sops-style-geometry.md](PLAN/MILESTONES/M54-sops-style-geometry.md) |`

  ```
  # Milestone 54: SOPs-style geometry — GeoDetail data kind, bridges, operators, point editing

  Stage 3 of the 3D roadmap (`PLAN/DESIGN/2026-09-18-3d-roadmap-lops-paint-sops.md`,
  Question 3). A fourth data kind, `eDataKindGeometry`, carrying a
  copy-on-write `GeoDetail` (UsdGeom's attribute vocabulary without a
  stage); explicit bridges to and from the scene stack; the same
  `Viewport3D` for display via a scratch stage; a Nuke-classic-sized
  operator set plus a Python wrangle; and EditGeo-style point editing.
  Decision: `PLAN/DECISIONS/2026-09-18-geometry-is-a-fourth-data-kind.md`.
  Requires M52 (`PrimSelector`) and M20.P2.T1 (gizmo). Runs after M53.
  All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Geometry/`,
  registered under `PLUGIN_GROUP_3D`; mirror M18's deep plumbing wherever a
  choice arises.

  ## Phase 54.1: Data kind, container, cache, pull path

  - [ ] M54.P1.T1 — `eDataKindGeometry` declared and drawn
    - files: `Global/Enums.h`, `Gui/Edge.cpp`, `Gui/NodeGui.cpp`, `Tests/DataKind_Test.cpp`, `Engine/Nodes/README.md`
    - approach: add the fifth `DataKindEnum` value; edge styling and node silhouette per the 2026-09-05 design doc's "Communicating kind" rules (distinct from image/deep/scene); connection typing needs no change — it is kind-agnostic. Document the kind in the README's "Data kinds" section with the same "declaration only until 54.1 lands" caveat deep carried.
    - verify: `DataKind_Test` extended — a geometry source into a scene input is refused, geometry → `Dot` → geometry consumer resolves; Xvfb GUI screenshot shows the new edge style.
    - size: S

  - [ ] M54.P1.T2 — `GeoDetail` value type
    - files: `Engine/GeoDetail.h`/`.cpp` (pimpl, no pxr header), `Engine/Nodes/Geometry/GeoDetailImpl.h`/`.cpp`, `Tests/GeoDetail_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: per the design doc's container sketch — `prims` (kind mesh/points/curves, path token, `VtArray` points/counts/indices, attribute map keyed by (interpolation, name) → `VtValue`, groups name → bitset), `contentHash` (U64 over all arrays, computed lazily and cached), `getSizeInBytes()` aliasing-blind like `DeepImage`. Mutation API returns a new detail sharing untouched arrays (`VtArray` COW). Public header exposes typed accessors (`const float*`/counts) so `Engine/` core and tests never see pxr.
    - verify: unit tests — copying then mutating `points` detaches only `points` (other arrays share storage, checked via `VtArray::IsIdentical`); identical content hashes identically; `Engine/GeoDetail.h` includes no pxr header (grep-enforced, as M19.P1.T2).
    - size: M

  - [ ] M54.P1.T3 — `Cache<GeoDetail>` with its own budget
    - files: `Engine/GeoDetailKey.h`/`.cpp`, `Engine/GeoDetailCacheEntry.h`/`.cpp`, `Engine/Settings.cpp`/`.h` (budget knob), `Tests/GeoDetailCache_Test.cpp`
    - approach: third instantiation of the templated cache, mirroring `DeepImageKey`/`DeepImageCacheEntry` from M18 exactly: key = (nodeHash, time, view); cost = `getSizeInBytes()`, a pure function of shape at allocate and deallocate (the 2026-09-09 amendment rule); separate memory budget knob so geometry never evicts the image cache.
    - verify: unit test — store/fetch by key; budget clamp evicts; counter returns to zero after eviction.
    - size: M

  - [ ] M54.P1.T4 — `renderGeometry()` capability virtual and pull path
    - files: `Engine/EffectInstance.h`/`.cpp`, `Engine/Nodes/NativeEffectBase.h`/`.cpp`, `Tests/GeoRender_Test.cpp`
    - approach: mirror the deep path M18 added (`renderDeep()` and its `renderDeepRoI`-shaped pull): `EffectInstance::renderGeometry(const GeoRenderActionArgs&, GeoDetailPtr*)` plus a pull that walks typed inputs, honours `getFramesNeeded()`, abort flags and the scheduler, consults `Cache<GeoDetail>` first, and ignores RoI (whole-detail pull, as scene). `NativeEffectBase` gains `renderGeometryFromInput(args, input, mutate)` (fetch input detail, hand a COW copy to `mutate`) and a generator variant. Trivial proof node `GeoConstant` (one triangle) used by the test only (`internalUseOnly = true`).
    - verify: unit test — `GeoConstant` → passthrough → consumer pulls the detail through the cache (second pull is a cache hit); abort mid-pull returns cleanly.
    - size: L

  ## Phase 54.2: Bridges and viewing

  - [ ] M54.P2.T1 — `GeoUsdConvert`: detail ↔ UsdGeom lookup table
    - files: `Engine/Nodes/Geometry/GeoUsdConvert.h`/`.cpp`, `Tests/GeoUsdConvert_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: both directions: detail prim ↔ `UsdGeomMesh`/`UsdGeomPoints`/`UsdGeomBasisCurves`, attributes ↔ primvars with interpolation, groups ↔ `UsdGeomSubset` (prim groups) or a `bool[]` primvar (point groups). Operates on an `SdfLayer`/`UsdPrim` at a given `UsdTimeCode`; no node logic here.
    - verify: round-trip unit test on an anonymous layer — a detail with a mesh, a `vertex` color attribute and a prim group authors a `UsdGeomMesh` with matching points/indices, a `primvars:color` with `vertex` interpolation and a `UsdGeomSubset`; converting back yields an identical detail (same content hash).
    - size: M

  - [ ] M54.P2.T2 — `GeoToScene`
    - files: `Engine/Nodes/Geometry/GeoToScene.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoToScene_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: inputs Geometry (+ optional Scene to append to); knobs: root scope path, "author time samples across range" (default off — vertex data is never auto-baked). Output kind **scene**: authors the prims into the node's layer via `SceneOps` and `GeoUsdConvert` at the current time; memoized per M19.P1.T3 keyed on the detail hash.
    - verify: unit test — a `GeoConstant` detail appears under the scope path on the composed stage; re-evaluating with an unchanged detail re-emits the same layer handle.
    - size: M

  - [ ] M54.P2.T3 — `SceneToGeo`
    - files: `Engine/Nodes/Geometry/SceneToGeo.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/SceneToGeo_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: inputs Scene; knobs: `PrimSelector`, "flatten transforms" (bake world xform into points), time follows the frame. Uses `GeoUsdConvert` over the composed input stage. Prims of unsupported types are skipped and listed in the node's info message.
    - verify: unit test — `ReadScene`(reference `.usda`) → `SceneToGeo` yields the expected mesh; with flatten on, a translated prim's points move; `GeoToScene` → `SceneToGeo` is the identity on a detail.
    - size: M

  - [ ] M54.P2.T4 — `Viewport3D` displays geometry nodes through a scratch stage
    - files: `Gui/Viewport3D.cpp`/`.h`, `Engine/Nodes/Geometry/GeoScratchStage.h`/`.cpp`
    - approach: when the viewed node's effective kind is geometry, pull its detail, convert via `GeoUsdConvert` into an anonymous layer under `/Natron/geo`, and hand the resulting stack to the existing viewport path (`StageCache` keyed by detail hash, so unchanged geometry costs nothing). Picking (M19.P2.T3) returns the prim path, which maps back to the detail's prim index by table.
    - verify: manual — a `GeoPrimitive` node viewed in `Viewport3D` shows the mesh; scrubbing a time-varying upstream node updates it; clicking a prim reports its index.
    - size: M

  ## Phase 54.3: Operators

  - [ ] M54.P3.T1 — `GeoPrimitive`
    - files: `Engine/Nodes/Geometry/GeoPrimitive.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoPrimitive_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: generator; type knob `card | grid | cube | sphere | cylinder`, size/segment knobs per type, emits `st` (`faceVarying`) and normals. Card is Nuke's Card: a single quad in XY with optional subdivisions.
    - verify: unit test — each type yields the expected point/face counts and a closed index buffer; UVs cover 0–1.
    - size: M

  - [ ] M54.P3.T2 — `GeoImportOBJ`
    - files: `Engine/Nodes/Geometry/GeoImportOBJ.h`/`.cpp`, `Engine/Nodes/Geometry/ObjParser.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoImportOBJ_Test.cpp`, a tiny committed `.obj`
    - approach: minimal OBJ parser (v/vt/vn/f with n-gons, groups → prim groups, objects → prims); USD/Alembic import is `ReadScene` → `SceneToGeo`, so this node is OBJ only. Missing file sets a node error.
    - verify: unit test — the committed `.obj` loads with matching counts, UVs and two groups.
    - size: M

  - [ ] M54.P3.T3 — `GeoTransform`
    - files: `Engine/Nodes/Geometry/GeoTransform.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoTransform_Test.cpp`
    - approach: TRS + pivot knobs (same layout as `Transform3D`), optional group restriction; transforms points and normals (COW copy of those two arrays only, everything else stays shared with the input).
    - verify: unit test — transform moves points; `st` storage is identical to the input's (`VtArray::IsIdentical`).
    - size: S

  - [ ] M54.P3.T4 — `GeoMerge`
    - files: `Engine/Nodes/Geometry/GeoMerge.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoMerge_Test.cpp`
    - approach: multi-input concatenation of prims in input order; prim-path collisions get a numeric suffix; groups keep their names per prim.
    - verify: unit test — merge of two details has both prims with unique paths and each input's groups intact.
    - size: S

  - [ ] M54.P3.T5 — `GeoGroup`
    - files: `Engine/Nodes/Geometry/GeoGroup.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoGroup_Test.cpp`
    - approach: creates a named point or prim group from one of: a bounding box, a prim-name pattern (`PrimSelector` syntax over prim paths), or an attribute comparison (`attr op value`, scalar attributes). Output is the input with the group added (COW: only the groups map changes).
    - verify: unit test — bbox group of a grid selects exactly the left-half points; a prim pattern selects exactly the matching prim; an attribute comparison over a `vertex` float attribute selects the expected set.
    - size: M

  - [ ] M54.P3.T6 — `GeoDelete`
    - files: `Engine/Nodes/Geometry/GeoDelete.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoDelete_Test.cpp`
    - approach: removes the members of a named group, or its complement. Points: drop the points and every face touching them, re-index faces and every `vertex`/`faceVarying` attribute. Prims: drop them. Filtered copy.
    - verify: unit test — deleting a grid's left-half group leaves the right half with a valid, re-indexed buffer and consistent attribute lengths; "delete non-selected" gives the complement.
    - size: M

  - [ ] M54.P3.T7 — `GeoNormals`
    - files: `Engine/Nodes/Geometry/GeoNormals.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoNormals_Test.cpp`
    - approach: computes area-weighted vertex normals as a `vertex` `normals` attribute, optionally face normals as a `uniform` attribute; exposes the computation as a static helper other nodes (`GeoDisplace`, `GeoEdit`) call after moving points.
    - verify: unit test — normals of a cube are unit length and outward; a flat grid's are all +Z.
    - size: S

  - [ ] M54.P3.T8 — `GeoDisplace`
    - files: `Engine/Nodes/Geometry/GeoDisplace.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoDisplace_Test.cpp`
    - approach: moves points along normals by a scalar from a noise knob or an optional **Image** input sampled at `st` (luminance), with amplitude and offset knobs and optional group restriction; recomputes normals via `GeoNormals`' helper afterwards; computes normals first if the input has none.
    - verify: unit test — a flat grid displaced by a constant image moves every point by the same amount along +Z and its normals stay +Z.
    - size: M

  - [ ] M54.P3.T9 — `GeoSubdivide` via OpenSubdiv
    - files: `Engine/Nodes/Geometry/GeoSubdivide.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoSubdivide_Test.cpp`, CMake (link OpenSubdiv, already a USD dependency)
    - approach: Catmull-Clark or Loop, level knob 1–4, interpolates `st` and other `vertex`/`faceVarying` attributes through the refiner; prim groups survive as face subsets. Uses OpenSubdiv's Far refiner directly rather than Hydra.
    - verify: unit test — cube at level 1 has the expected face count and stays closed; `st` is interpolated.
    - size: M

  - [ ] M54.P3.T10 — `GeoUVProject`
    - files: `Engine/Nodes/Geometry/GeoUVProject.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoUVProject_Test.cpp`
    - approach: writes `st` from a planar (axis + TRS) or camera projection (inputs: optional Scene for the camera; camera path knob), `faceVarying` interpolation. Pairs with M53's `ProjectTexture` for geometry with no UVs.
    - verify: unit test — planar projection of a grid yields `st` equal to its normalized XY; camera projection of a camera-facing quad yields 0–1.
    - size: S

  - [ ] M54.P3.T11 — `GeoWrangle`: Python over the detail's arrays
    - files: `Engine/Nodes/Geometry/GeoWrangle.h`/`.cpp`, `Engine/Nodes/Geometry/GeoPyBuffer.h`/`.cpp`, `Engine/AppManager.cpp`, `Tests/GeoWrangle_Test.cpp`
    - approach: a script knob (multi-line `KnobString`) run in Natron's embedded Python under the GIL during `renderGeometry()`, with a `geo` object exposing each prim's arrays via the CPython buffer protocol (a small hand-written extension type, no Shiboken typesystem changes, no pxr Python): `geo.prims[i].points` is a writable float `memoryview` of shape (N,3); attributes likewise by name; `geo.prims[i].group("name")` a bool view. Writes go to the COW-detached output copy. numpy is optional (`np.frombuffer(view)` if present). Script errors become a node error with the traceback.
    - verify: unit test — script `for p in geo.prims: pts = p.points; ...` doubling every Z runs and doubles Z; a syntax error reports on the node without aborting the render thread.
    - size: L

  ## Phase 54.4: Point editing

  - [ ] M54.P4.T1 — `Viewport3D` point picking
    - files: `Gui/Viewport3D.cpp`/`.h`
    - approach: a points pick mode using `UsdImagingGLEngine::TestIntersection` with the pick target set to points (Storm renders point-id AOVs for this), click and marquee select, selection stored as (prim path, point index) set and drawn as highlighted points. Available only when the viewed node's kind is geometry.
    - verify: manual — clicking a vertex of a `GeoPrimitive` cube highlights it; marquee selects the expected set.
    - size: M

  - [ ] M54.P4.T2 — `GeoEdit` node with viewport drag
    - files: `Engine/Nodes/Geometry/GeoEdit.h`/`.cpp`, `Gui/Viewport3D.cpp` (drag → knob writeback), `Engine/AppManager.cpp`, `Tests/GeoEdit_Test.cpp`
    - approach: stores a sparse map `(prim index, point index) → offset` in a serialized knob (a `KnobString` holding a compact text encoding is acceptable; a table knob if one fits); render applies offsets to points (COW-detaching `points` only) and recomputes normals if present. Viewport: with the node selected and points picked (T1), M20.P2.T1's translate gizmo drags them; release writes one knob change (one undo entry). Topology guard: if the input's point count for a prim differs from the count recorded when the edit was made, skip that prim's edits and set a node warning.
    - verify: unit test — an edit map round-trips through serialization and moves exactly the listed points; changing the upstream point count triggers the warning and leaves points unmodified. Manual — drag creates one undo entry.
    - size: L

  ## Phase 54.5: Integration

  - [ ] M54.P5.T1 — Geometry pipeline integration test in CI
    - files: `Tests/GeoPipeline_Test.cpp`, `Tests/CMakeLists.txt`
    - approach: headless graph — `GeoPrimitive`(grid) → `GeoTransform` → `GeoGroup`(bbox) → `GeoDelete` → `GeoDisplace`(constant) → `GeoNormals` → `GeoEdit`(one point) → `GeoToScene` → `Merge3D`(with `ReadScene`) → `WriteScene`; assert the exported mesh's point count, one edited point's position, and the `primvars:normals`. Plus `SceneToGeo` of the same export equals the pre-bridge detail.
    - verify: test green in `build-and-test`.
    - size: S

  **Verification gate:** CI green including M54.P5.T1; on real hardware a geometry chain displays in `Viewport3D`, point picking and `GeoEdit` drag work with undo, and the result renders through `GeoToScene` → `HydraRender` to an image; `GeoWrangle` runs a script that edits points; geometry cache respects its own budget; pre-existing ctest suite green; `Engine/GeoDetail.h` and `Engine/` core stay pxr-free.
  ```

### 2026-09-19T10:45:00-04:00 — change-request
- refs: M43, M34, M35, M36, M37, M38
- Move M43 (Drop the premultiplied/unpremultiplied concept) ahead of M34 in the board. New sequence: M57, M35, M38, M43, M34, M36, M37, M50 (compositing semantics integrated into the channel/layer rework)

### 2026-10-04T23:15:35-04:00 — answer
- refs: M61, M65, M66, M37, M50, M60, M62
- Parcel UAT result from the user: "These all look good." The user walked through the UAT for the stacked PRs #34 (M61), #35 (M65), #36 (M66), #37 (M37), #38 (M50), #39 (M60) and #40 (M62), and says they are OK to merge, bottom-up (#34 first). This answers the parcel-UAT items under `# Open questions` for all seven milestones (M61 sign-off, M65 P8.T3, M66 P6.T3, M37 P4.T3/P6.T2, M50 P6.T2, M60 P5.T1/P6.T2, M62 scale check). Check off the UAT-gated tasks, move the `blocked` rows to `done` once each PR merges, and remove the answered questions. The fork PRs (openfx-misc#5/#6, openfx-io#8/#9/#10, openfx-arena#2/#3/#4, openfx-natron#3) merge with their milestones, followed by re-pinning `tools/ci/local/fetch-assets.sh` and `libs/OpenFX`. After #40, re-run `tools/bench/run_matrix.sh` on `main` to refresh `BASELINE.md`.

### 2026-10-08T04:45:00+00:00 — change-request
- refs: M69 (new), M55, M56, M67
- Add a new milestone M69 "Minor cleanup" as a **stub**, todo, placed at the end of the board (no execution order set; user said "some later" milestone). Create `PLAN/MILESTONES/M69-minor-cleanup.md` with the content below and add the board row `| M69 | Minor cleanup: keyer redesign, app-level hidpi/interactive, node-graph input handling, curve-editor handles | todo | [M69-minor-cleanup.md](PLAN/MILESTONES/M69-minor-cleanup.md) |`. Do not start or elaborate without a user go-ahead.

````markdown
# Milestone 69: Minor cleanup

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A grab-bag of small UX and design fixes from the user (2026-10-08), grouped to ship together. Each item is independent.

Keying and node knobs:
- **Keyer redesign.** The current design is unintuitive. It becomes four values: below A the matte is 0, between B and C it is 1, above D it is 0 (a trapezoid, with A<=B<=C<=D). It only creates an alpha: no premult, no compositing over a background, no despill. The native Keyer from M67.P5.T1 is the node to redo. Decide at elaboration whether ChromaKeyer shares the new matte stage.
- **No `hidpi` knob on nodes.** DPI handling becomes an app-level concern. Remove the knob from every node that carries it (the native generators and Transform-family nodes from M67 included) and handle scaling in the app (preference or automatic).
- **`interactive` is not a node property.** Replace the per-node knob with a global toggle with a keyboard shortcut. Remove the knob from nodes (M67's native nodes included).
- **Move "(un)premult by" below the line** with mask and mix, as should clamp black/clamp white on the nodes that have those knobs.

Node graph interaction:
- **Paste attaches.** Pasting a node while another node is selected connects the pasted node's main input to the selected node.
- **Unconnected inputs stay visible** when the node is deselected (they currently disappear).
- **Inputs are easier to grab.** Unconnected input arrows have a clunky, small hitbox; enlarge it.
- **Shortcuts for connecting node inputs.**
- **Pick-up connection mode.** Clicking an input "picks it up" and it sticks to the cursor; clicking a node then sets that input to the clicked node. Panning and zooming still work while picked up; right-click or Escape puts it down (cancels).
- **Tab menu position.** The tab menu briefly opens at the extreme top-left and then jumps to the cursor; it must open at the right position the first time.

Curve editor:
- **Full handle control.** Dragging a handle lengthens/shortens it; handles can be broken (independent in/out tangents) and re-joined.

Blocked on: a codebase-scouting pass (graph input arrow hit-testing, the tab menu show path, curve-editor tangent model, where `hidpi`/`interactive` are consumed), and a design answer on where the global interactive toggle and the app-level DPI setting live. The keyer and knob items should wait until M67 has landed and been UAT'd, since they edit nodes that milestone writes. Overlaps with M55 (node-graph interaction) and M56 (the unpremult-by label); decide at elaboration whether to fold those items in or leave them.

Acceptance sketch:
- Keyer has four matte values and outputs an alpha only.
- No node has `hidpi` or `interactive` knobs; DPI is app-level and interactive is one global toggle with a shortcut.
- Pasting onto a selected node connects it; unconnected inputs remain visible when deselected and are easy to grab.
- Pick-up mode connects an input with a click, pans/zooms while held, and cancels on right-click/Escape.
- Tab menu opens in the right spot with no jump.
- Curve-editor handles can be dragged, lengthened, shortened and broken.
````

### 2026-10-08T12:15:00+00:00 — change-request
- refs: M69
- Append a new section to the M69 stub (apply after the M69-creation entry above) and add one line to its intent. Stub stays a stub; do not elaborate without a user go-ahead. Section to append to `PLAN/MILESTONES/M69-minor-cleanup.md`, under the existing groups:

````markdown
Viewer colour picker:
- **Pick scene-referred values.** The picker's tooltip says it reads the display-referred value from the viewer and converts it back to linear. That is wrong: it must return the scene-referred value at the picked pixel, before any viewer transform (display/view, gain, gamma, channel display). Currently `ViewerGL::getColorAt` / `getColorAtRect` (Gui/ViewerGL.cpp) take a `linear` flag from the `getColorPickerLinear` setting; confirm at elaboration whether the values are read from the post-transform texture, and fix so the picker samples the pre-transform image data. The tooltip and the `getColorPickerLinear` setting are then likely obsolete.
- **Pick from a node's input or output.** Add a viewer picker mode that reads the colour on the viewed node's **input** or on its **output**. The default is the node's **input**, the opposite of Nuke (where it is the output). Decide at elaboration where the mode lives (viewer toolbar or the picker's own control) and how it behaves when the viewed node has several inputs (the one feeding the viewer's active input, or the first).
````

### 2026-10-09T07:58:00-04:00 — change-request
- refs: M80, M81
- New milestones from a /cat-plan session: add RAW support back by porting darktable's decoding and processing modules. User decisions (2026-10-09): decoder is rawspeed vendored as a pinned dependency (no LibRaw); the Read emits the undemosaiced mosaic (not a separate RawRead node, not a pre-demosaiced image); darktable modules ported are the core chain plus extras (hot pixels, chromatic aberrations, raw denoise, lens correction); GPU is in scope but split into M81 so the CPU work can ship first. Please: (1) add a project-wide decision `DECISIONS/2026-10-09-raw-support-via-rawspeed-and-darktable-ports.md` recording the above and that it supersedes the RAW-dropped clause of `2026-10-09-native-read-format-scope.md`, with an INDEX line; (2) append two board rows after M79: `| M80 | Native RAW Support | todo | [M80-native-raw-support.md](PLAN/MILESTONES/M80-native-raw-support.md) |` and `| M81 | Raw GPU Kernels | todo | [M81-raw-gpu-kernels.md](PLAN/MILESTONES/M81-raw-gpu-kernels.md) |`, placing them after M79 - Layered Document Readers in execution order; (3) create the two milestone files below verbatim. M80 - Native RAW Support depends on M75 - Native Read shipping; M80.P2.T2 may end with an open question if a source node cannot resolve its data kind from the file, in which case the fallback is a separate RawRead node and the user must be asked.

#### FILE: PLAN/MILESTONES/M80-native-raw-support.md
~~~markdown
# M80 - Native RAW Support

Brings RAW camera files back as a native pipeline ported from darktable: rawspeed decodes the file, the native Read emits the undemosaiced sensor mosaic on a new `eDataKindRaw` edge, and native nodes ported from darktable's CPU modules (rawprepare, temperature, highlights, demosaic, input colour, plus hot pixels, chromatic aberrations, raw denoise and lens correction) turn it into a scene-linear image. This supersedes the "RAW camera formats are dropped" clause of `DECISIONS/2026-10-09-native-read-format-scope.md`; the "different technique, not OIIO's libraw plugin" it anticipated is rawspeed plus these nodes. GPU kernels are out of this milestone: Phase 80.6 only decides the backend, and M81 - Raw GPU Kernels does the ports. Licensing: darktable's modules are GPL-3.0-or-later, rawspeed is LGPL-2.1, and Natron is GPL-2-or-later, so ported files keep their darktable copyright headers and the combined work is distributed under GPL-3 terms; record this in `tools/license/components/README.md`. Depends on M75 - Native Read shipping (the reader structure) and on M74 - Native Metadata Core (the mosaic parameters travel as image metadata).

## Phase 80.1: Foundation

- [ ] M80.P1.T1 — Vendor rawspeed as a pinned dependency
  - files: `tools/ci/local/fetch-assets.sh`, `Engine/CMakeLists.txt`, `tools/license/components/README.md`, `tools/release/stage-bundle.sh` (only if the shared object needs bundling)
  - approach: build rawspeed (and its pugixml dependency, if the image lacks it) from source at an exact commit SHA, following the `LCMS2_REPO`/`LCMS2_REF` recipe (`fetch-assets.sh:173-174,286-330`) into `build/assets/plugin-src/deps-install/`; static if rawspeed supports it, so the AppImage needs nothing new. `find_package` or pkg-config it in `Engine/CMakeLists.txt` and link it. Add the licence entries (rawspeed LGPL-2.1, pugixml MIT, the darktable GPL-3 note). Verify the dev image first: `docker ps`, then check what the image already ships before building anything. Include rawspeed's `cameras.xml` in the bundle; the decoder needs it at run time.
  - verify: a one-line test binary links rawspeed and `RawDecoder` constructs; the fetch step is idempotent on a second run; the licence README names both libraries.
  - size: M

- [ ] M80.P1.T2 — Add the `eDataKindRaw` data kind and its Raw-to-Image adapter row
  - files: `Global/Enums.h`, `Engine/Node.cpp` (kind switches, `isRegisteredAdapter` at ~1026), `Gui/Edge.cpp`, `Gui/NodeGui.cpp`, `Engine/Nodes/README.md`
  - approach: add `eDataKindRaw` to `DataKindEnum` (`Global/Enums.h:514`). The payload is a one-channel float `Image` holding the CFA mosaic in the existing image cache, so no new render virtual or cache (unlike Deep's `EffectInstanceRenderDeep.cpp` path); the kind exists for edge typing only. Add the adapter row so a raw output can feed an Image input only through a node that declares raw in and image out (demosaic), not implicitly. Colour the edge and the node tint with a new Okabe-Ito colour distinct from Deep's blue (`Gui/Edge.cpp:788-811`, `Gui/NodeGui.cpp:189-196`). Check `Engine/EffectInstance.cpp:5210`, `Engine/OutputSchedulerThread.cpp` (2402-2648) and `Engine/ViewerInstance.h:454` for switches that need a raw arm. Document the kind in the README "Data kinds" section, including that the mosaic's layout travels as metadata (P1.T3).
  - verify: gtest: a raw output cannot connect to an image-only input (`eCanConnectInput_incompatibleDataKind`) and can connect to a raw input; a project with a conflicting edge reports it on load via `reportDataKindConflicts`; full ctest unchanged.
  - size: M

- [ ] M80.P1.T3 — Define the raw mosaic metadata schema
  - files: new `Engine/Nodes/IO/RawMetadata.h/.cpp`, new `Tests/Native/RawMetadata_Test.cpp`
  - approach: typed accessors over the M74 metadata store (`Engine/Nodes/Metadata/ImageMetadata.h`) for the keys every raw node needs: `raw/cfa_pattern` (2x2 Bayer or 6x6 X-Trans as a string plus the pattern origin offset), `raw/black_levels` (per CFA colour), `raw/white_level`, `raw/wb_coeffs` (as shot), `raw/cam_to_xyz` (3x3 D65 matrix from rawspeed's camera database), `raw/crop` (active area), `raw/iso`, `raw/exposure`, `raw/stage` (`raw`, `prepared`: scaled to 0-1 with black subtracted). Provide `RawLayout` (pattern period, colour-at(x, y) with the crop offset applied) so every node shares one definition. A node that crops or shifts the window must update the pattern origin; document that.
  - verify: gtest: round-trip every key; `colourAt` matches a hand table for RGGB, BGGR, GRBG, GBRG and a 6x6 X-Trans, including after a one-pixel crop offset.
  - size: M

## Phase 80.2: Decode and read

- [ ] M80.P2.T1 — rawspeed decode support
  - files: new `Engine/Nodes/IO/RawDecodeSupport.h/.cpp`, new `Tests/Native/RawDecodeSupport_Test.cpp`, new fixtures under `Tests/fixtures/raw/`
  - approach: model it on `OiioReadSupport` (P1.T2 of M75 - Native Read): a header cache keyed on (path, mtime, size) holding the parsed `RawImage` metadata, behind a mutex, and `decode(path, RectI window, float* dst, stride)` copying the window of the mosaic as float. rawspeed decodes whole files, so keep one decoded `RawImage` per cache key with a size-bounded LRU (many-core, high-RAM and modest machines alike: the cap is a fraction of the project cache, not a constant). Fill `RawMetadata` from the decoder (CFA, black/white levels, as-shot white balance, camera matrix from `cameras.xml`, crop). All offsets `size_t`. Fixtures: two or three small CC0 raw files (a Bayer DNG, an X-Trans RAF or similar) from the public raw.pixls.us samples; record their source in `Tests/fixtures/raw/README.md`.
  - verify: gtest: dimensions, CFA pattern, levels and crop match rawspeed's own output for each fixture; a window decode equals the same window of the full decode bit for bit; a truncated file fails cleanly.
  - size: M

- [ ] M80.P2.T2 — Let a native Read resolve its output kind from the file
  - files: `Engine/Nodes/NativeEffectBase.h/.cpp`, `Engine/Node.cpp` (kind resolution ~1051-1152), `Engine/Nodes/README.md`, new `Tests/Native/NativeSourceKind_Test.cpp`
  - approach: kinds are static plugin declarations today; `resolveOutputDataKind` (`NativeEffectBase.h:297`) exists for polymorphic nodes and keys off inputs. Read has no input and is `eDataKindImage`. Scout first and report: the least invasive way for a source node to declare a polymorphic output whose kind comes from a knob-derived value (the file extension), including cache invalidation when `filename` changes (`Node.cpp:~1363`) and what happens to downstream edges whose kind flips (they must go through the existing conflict reporting, not be disconnected). Implement that. If it cannot be done without changing every node, stop and report as an open question: the fallback is a separate `RawRead` node, and the user chose Read-emits-mosaic.
  - verify: gtest: a test source node whose kind follows a string knob flips Image to Raw; the downstream raw-only node reports a conflict when it flips back; a project save/load round trip keeps the resolved kind.
  - size: L

- [ ] M80.P2.T3 — Read renders the mosaic for RAW files
  - files: `Engine/Nodes/IO/NativeRead.cpp`, `Engine/Nodes/IO/OiioReadSupport.cpp` (extension list only), `Engine/AppManager.cpp` (RAW extension set), new `Tests/Native/NativeReadRaw_Test.cpp`
  - approach: a constant set of rawspeed's supported extensions (cr2, cr3, nef, arw, raf, rw2, orf, dng, pef, srw and the rest rawspeed lists) is claimed by the native Read before OIIO's `extension_list`, which still must not hand them to its libraw plugin. For a RAW file the Read outputs `eDataKindRaw`: one float channel, the active-area window as the RoD, the mosaic from `RawDecodeSupport`, and the `RawMetadata` keys merged into the file metadata (`deriveOutputMetadata`). `ocioInputSpace` is not applied to mosaic data: hide the colour knobs for a raw file. Frame sequences, proxy and the time domain work as for images (the proxy is disabled for raw: downscaling a mosaic is meaningless; say so in the tooltip).
  - verify: gtest: Read of each fixture is raw-kind, one channel, with the expected metadata keys and mosaic values; a non-RAW file still outputs an image; switching `filename` between a PNG and a RAW file changes the kind and the downstream edge conflict is reported.
  - size: M

- [ ] M80.P2.T4 — View the mosaic directly
  - files: `Engine/ViewerInstance.cpp/.h` (`:454-459` region), `Engine/Nodes/README.md`
  - approach: let the viewer accept a raw-kind input and display it as a greyscale single channel, by treating the mosaic as a one-channel image (the payload already is one). No demosaic in the viewer. Show "RAW mosaic" in the viewer's info line.
  - verify: gtest: a viewer connected to a raw Read renders without error and the pixel values equal the mosaic; the `Tests/gui` smoke script is extended to show it.
  - size: M

## Phase 80.3: Core develop chain (CPU)

Each port keeps the darktable source file named in `approach` as the reference, ports the C path only (never the SSE/OpenCL variants; those return in M81 - Raw GPU Kernels), and keeps the darktable copyright header. Every node declares raw in and raw out unless stated, uses the row-band helpers per the Nodes README "Threading" rules, and copies `Blur`'s RoI pattern (`getSourceRoI`, `getRegionsOfInterest`) where it needs neighbours. Nodes register in the Tab menu under a `Raw` group.

- [ ] M80.P3.T1 — RawPrepare node (levels, scaling, crop)
  - files: new `Engine/Nodes/Raw/RawPrepare.h/.cpp`, `Engine/AppManager.cpp` (one registration), new `Tests/Native/RawPrepare_Test.cpp`
  - approach: port darktable `src/iop/rawprepare.c` (C path). Subtract per-CFA-colour black levels, scale by the white level to 0-1, apply the crop (shifting the pattern origin via `RawLayout`), set `raw/stage` to `prepared`. Knobs: black levels (override), white level (override), crop margins. Defaults come from the metadata.
  - verify: gtest against a hand-computed mosaic with known levels; a crop by an odd number of pixels keeps `colourAt` correct.
  - size: M

- [ ] M80.P3.T2 — Temperature node (white balance on the mosaic)
  - files: new `Engine/Nodes/Raw/RawTemperature.h/.cpp`, registration, new `Tests/Native/RawTemperature_Test.cpp`
  - approach: port the raw-domain part of darktable `src/iop/temperature.c`: per-CFA-colour gains applied to the prepared mosaic. Knobs: temperature, tint, the three channel coefficients, and a mode (as shot, camera reference, custom). The temperature-to-coefficient conversion uses the camera matrix from metadata, as darktable does; port `dt_colorspaces_...` helpers it needs locally rather than pulling darktable's colour library.
  - verify: gtest: as-shot coefficients reproduce rawspeed's; a custom temperature on a synthetic grey patch with a known matrix gives equal demosaiced channels (checked after P3.T4).
  - size: M

- [ ] M80.P3.T3 — Highlights node (clip and reconstruct)
  - files: new `Engine/Nodes/Raw/RawHighlights.h/.cpp`, registration, new `Tests/Native/RawHighlights_Test.cpp`
  - approach: port darktable `src/iop/highlights.c` C path: clip, unclipped (leave), and LCh reconstruction where the mosaic allows (the "inpaint opposed" mode of newer darktable if the source tree has it; pin the darktable commit this task ports from in the file header). Needs the white level and white-balance coefficients from metadata for the clip threshold.
  - verify: gtest: a synthetic mosaic with a clipped patch, each mode matching a reference computed from the ported algorithm on the same input (reference generated once from darktable built in a throwaway container and stored as a small fixture; record how in the fixture README).
  - size: M

- [ ] M80.P3.T4 — Demosaic node: PPG and RCD (Bayer)
  - files: new `Engine/Nodes/Raw/RawDemosaic.h/.cpp`, new `Engine/Nodes/Raw/DemosaicBayer.h/.cpp`, registration, new `Tests/Native/RawDemosaicBayer_Test.cpp`
  - approach: port darktable `src/iop/demosaic.c` and `src/iop/demosaicing/ppg.c` and `rcd.c`. Raw in, image out (the registered adapter from M80.P1.T2): three colour channels plus alpha 1. Halo from the algorithm (RCD needs 4-6 px) rounded up to the pattern period; the RoD equals the input's. Knobs: method, green-equilibration, colour smoothing passes. The demosaic node is the only node that does this; its method knob is an enum so P3.T5 and T6 add choices.
  - verify: gtest: a synthetic gradient mosaic reconstructs within a bound; a tile rendered with RoI expansion equals the same pixels of the full-frame render bit for bit (tiling safety); output kind is image.
  - size: L

- [ ] M80.P3.T5 — Demosaic: AMaZE
  - files: `Engine/Nodes/Raw/DemosaicAmaze.h/.cpp`, `Engine/Nodes/Raw/RawDemosaic.cpp` (method enum), new `Tests/Native/RawDemosaicAmaze_Test.cpp`
  - approach: port darktable `src/iop/demosaicing/amaze.cc` (GPL-3). It works in 128-px tiles with its own overlap: map that onto Natron's RoI (halo 18 px, pattern-aligned), not onto a whole-image buffer.
  - verify: gtest: tiled equals full-frame on a fixture; result within a bound of PPG's on a smooth patch and measurably sharper on an edge pattern.
  - size: L

- [ ] M80.P3.T6 — Demosaic: X-Trans (Markesteijn)
  - files: new `Engine/Nodes/Raw/DemosaicXTrans.h/.cpp`, `Engine/Nodes/Raw/RawDemosaic.cpp`, new `Tests/Native/RawDemosaicXTrans_Test.cpp`
  - approach: port darktable `src/iop/demosaicing/xtrans.c` (1-pass and 3-pass Markesteijn) and the fast VNG variant for X-Trans previews. The 6x6 pattern period drives the halo alignment.
  - verify: gtest: X-Trans fixture, tiled equals full-frame; output has no pattern artefacts on a flat patch.
  - size: L

- [ ] M80.P3.T7 — Input colour node (camera to working space)
  - files: new `Engine/Nodes/Raw/RawInputColor.h/.cpp`, `Engine/Project.cpp/.h` (only if a working-space accessor is missing), registration, new `Tests/Native/RawInputColor_Test.cpp`
  - approach: image in, image out, placed after demosaic. Camera RGB to XYZ D65 through the metadata matrix (`raw/cam_to_xyz`, user-overridable, plus an optional ICC/DCP-less "standard matrix" fallback), then to the project working space through the project OCIO (`DESIGN/2026-10-02-project-ocio.md`): build an OCIO matrix transform to the working space rather than hard-coding primaries. Port the matrix handling from darktable `src/iop/colorin.c`; do not port its ICC profile path.
  - verify: gtest: a grey card mosaic with the metadata matrix ends neutral in the working space; changing the project working space re-renders.
  - size: M

- [ ] M80.P3.T8 — RawDevelop preset: Read to a gradable image in one step
  - files: `Engine/AppInstance.cpp` or the node-creation hook found while scouting, `Engine/Nodes/Raw/RawDevelop.cpp` (or a Python gizmo under the existing gizmo mechanism: choose after scouting), `Tests/gui/m80_raw.py`
  - approach: since Read emits the mosaic, add a one-action "Develop RAW" that inserts RawPrepare, Temperature, Highlights, Demosaic and InputColor after a raw Read with the darktable defaults, as a group. It is a convenience: each node stays individually usable. Prefer a user-visible Group over a hidden composite node so the user can open and edit it.
  - verify: the Xvfb script creates a Read on a fixture, runs Develop, and the viewer shows a plausible colour image; ctest unchanged.
  - size: M

## Phase 80.4: Extra modules

- [ ] M80.P4.T1 — Hot pixels node
  - files: new `Engine/Nodes/Raw/RawHotPixels.h/.cpp`, registration, new `Tests/Native/RawHotPixels_Test.cpp`
  - approach: port darktable `src/iop/hotpixels.c`: threshold and strength, runs on the prepared mosaic with a 2-px halo.
  - verify: gtest: injected hot pixels on a smooth mosaic are removed and a clean mosaic is bit-identical.
  - size: M

- [ ] M80.P4.T2 — Chromatic aberration node
  - files: new `Engine/Nodes/Raw/RawCACorrect.h/.cpp`, registration, new `Tests/Native/RawCACorrect_Test.cpp`
  - approach: port darktable `src/iop/cacorrect.c` (Bayer; X-Trans passes through with a message). It estimates shifts over the whole image, so it needs the full frame: cache the shift estimate per (input hash, parameters) instead of recomputing per tile, and make the RoI request the full input for estimation.
  - verify: gtest: a synthetic mosaic with a known channel shift is corrected within a bound; tiled equals full-frame.
  - size: L

- [ ] M80.P4.T3 — Raw denoise node
  - files: new `Engine/Nodes/Raw/RawDenoise.h/.cpp`, bundled noise-profile data under `Engine/Nodes/Raw/data/`, registration, new `Tests/Native/RawDenoise_Test.cpp`
  - approach: port darktable `src/iop/rawdenoise.c` (wavelet) and the profile-driven `denoiseprofile.c` non-local-means variant; start with profile data for the fixture cameras and read the rest from darktable's `noiseprofiles.json` (record the pinned copy and licence). Knobs: method, threshold or strength, ISO override.
  - verify: gtest: added Gaussian noise on a smooth mosaic is reduced measurably (PSNR improves); edges preserved within a bound.
  - size: L

- [ ] M80.P4.T4 — Lens correction: add the lensfun dependency
  - files: `tools/ci/local/fetch-assets.sh`, `Engine/CMakeLists.txt`, `tools/license/components/README.md`
  - approach: pin and build lensfun (LGPL-3 library, database CC-BY-SA) the same way as P1.T1 and bundle its database. Check the image for an existing lensfun first.
  - verify: a test binary opens the lensfun database and finds a known lens entry.
  - size: M

- [ ] M80.P4.T5 — Lens correction node
  - files: new `Engine/Nodes/Raw/RawLensCorrect.h/.cpp`, registration, new `Tests/Native/RawLensCorrect_Test.cpp`
  - approach: port darktable `src/iop/lens.cc`: distortion, vignetting and transverse chromatic aberration from the lensfun profile, with the camera and lens read from `raw/*` metadata (add the make, model and lens keys to `RawMetadata` in this task) and manual override knobs. Image in, image out, placed after demosaic. Distortion changes the geometry: implement `getRegionOfDefinition` and `getRegionsOfInterest` from the inverse mapping, with a bounded halo.
  - verify: gtest: a synthetic grid through a known distortion profile lands within a sub-pixel bound; tiled equals full-frame.
  - size: L

## Phase 80.5: Verification against darktable

- [ ] M80.P5.T1 — Reference comparison harness
  - files: new `Tests/Native/RawReference_Test.cpp`, new `tools/raw/make-reference.sh`, `Tests/fixtures/raw/README.md`
  - approach: a script that, in a throwaway container with darktable installed, develops each fixture with a fixed XMP sidecar (only the ported modules enabled, no tone mapping) to a 32-bit TIFF in the same working space; the TIFF reference is committed as a small fixture. The test runs the Develop chain on the same file and compares with a tolerance per method (PPG and RCD close, AMaZE and Markesteijn within a looser bound), reporting mean and max error.
  - verify: the test passes for each fixture; the README states the tolerances and why.
  - size: M

- [ ] M80.P5.T2 — Large-image and threading check
  - files: new `Tests/Native/RawLarge_Test.cpp`, `tools/bench/`
  - approach: a synthetic 100 MP mosaic (generated at run time, never committed) through the whole chain: no `int` overflow (`size_t` row arithmetic), memory bounded by the decode cache cap, and the chain scales across threads without lock convoy (check with the `tools/bench` scripts, per the render-scaling findings). Concurrent renders of two views of one file share one decode.
  - verify: gtest completes under a stated memory bound; bench shows time falling with thread count on the many-core box.
  - size: M

- [ ] M80.P5.T3 — Panels and viewer check under Xvfb
  - files: `Tests/gui/m80_raw.py`, node panel layouts in `Engine/Nodes/Raw/*.cpp` (labels and pages only)
  - approach: run the Xvfb recipe (`build/deeprepro/run-gui.sh`, `checkForUpdates` pre-seeded off): open a Bayer and an X-Trans fixture, Develop, switch demosaic methods, change temperature, and capture screenshots of each node's panel and the viewer. Share the screenshots and get the user's approval before sign-off (project rule).
  - verify: scripts pass under Xvfb; the screenshots are shared and the user approves them.
  - size: M

## Phase 80.6: GPU backend decision

- [ ] M80.P6.T1 — Spike: choose the GPU backend for raw kernels
  - files: new `PLAN/DECISIONS/` entry (written by the PM), spike code under `build/` only
  - approach: Natron's GPU path is OpenGL 2.0 compatibility with FBO/PBO and glad (`Engine/OSGLContext.*`, `Tools/generate_glad.sh`); no node under `Engine/Nodes/` touches it, and there is no OpenCL, CUDA or compute-shader support. Compare (a) OpenCL as a new dependency, as darktable does (reuses darktable's kernels nearly verbatim; adds a runtime dependency and a second device context), and (b) GL fragment or compute shaders on the existing context (no new dependency; compute needs a GL 4.3 context the host does not create today; kernels need rewriting). Port one kernel (the RCD demosaic or the white-balance step) both ways and measure on the available GPU; the container has no GPU under Xvfb, so measurements run on the host. Recommend, with the many-core/modest-machine range in mind (CPU stays the default, GPU optional).
  - verify: a decision record naming the backend, the measured numbers and the cost of the other option; the user confirms the choice before M81 - Raw GPU Kernels is elaborated.
  - size: M

**Verification gate:** `ctest` green including every new `Tests/Native/Raw*` and `NativeReadRaw` test; the darktable reference comparison (M80.P5.T1) passes within its stated tolerances on a Bayer and an X-Trans fixture; the large-image test (M80.P5.T2) passes; the Xvfb script (M80.P5.T3) passes and the user has approved the screenshots; the backend decision (M80.P6.T1) is confirmed by the user.
~~~

#### FILE: PLAN/MILESTONES/M81-raw-gpu-kernels.md
~~~markdown
# M81 - Raw GPU Kernels

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Ports the GPU variants of the M80 - Native RAW Support nodes: darktable's OpenCL kernels for demosaic (PPG, RCD, AMaZE, Markesteijn), white balance, highlights, hot pixels, denoise and lens correction, onto the backend M80's spike chooses. CPU stays the default and the reference; the GPU result must match it within a tolerance, and a machine with no usable device falls back silently.

Blocked on: M80 - Native RAW Support finishing, specifically the backend decision in M80.P6.T1 (OpenCL as a new dependency versus GL shaders on the existing context) and the user's confirmation of it.

Acceptance sketch:
- Each ported module has a GPU path selectable per node and by the project GPU setting, falling back to CPU on failure.
- GPU and CPU outputs agree within a stated tolerance on the M80 fixtures.
- A measured speed-up on at least the demosaic step on a real GPU.
~~~

### 2026-10-09T22:08:44-04:00 — change-request
- refs: M82, M83, M84, M85, M86, M87
- Add six new milestones (user-requested, from a 2026-10-09 side-session investigation of GPU rendering). Suggested board position: after M80 - Native RAW Support, with M82 and M83 ahead of M81 - Raw GPU Kernels (see the separate change-request about M80.P6.T1). Investigation findings that apply to all six:
  - **Current state.** GPU means OpenGL only, through the OFX GL suite. Only Shadertoy and the openfx-io OCIO plugins use it; no native node has a GPU path. In task-graph mode `RenderScheduler::executeTask` hard-codes `eStorageModeRAM` (`Engine/RenderScheduler.cpp:711`) and fails any task that leaks a texture or leaves a context bound (`:727-751`). Per-thread contexts don't share. So every GL node is an island: upload, render, then `glFinish` + `glReadPixels` back to RAM. The legacy path's GL-chain residency is lost. `renderRoI` refuses GL for multi-consumer nodes, time offsets and force-cache (`Engine/EffectInstanceRenderRoI.cpp:704-712`), because GL output is never cached. Textures are always RGBA32F. The M63 decision record's claim that "GL stays on the legacy path" is stale.
  - **Economics.** A 4K RGBA float frame is about 133 MB. A PCIe 4.0 round trip is about 11 ms (PCIe 5.0 about 5 ms). A point op is about 0.3–0.5 ms on GPU and 3–5 ms on a many-core CPU. One round trip costs as much as two or three CPU point ops, so per-node offload pays only for heavy nodes. The win is **residency**: keep connected GPU-capable regions on the device and pay transfers only at region boundaries. Fusion is second-order. Point ops need GPU versions mainly so they don't break a region. Masks do not block fusion: each mask is an extra per-pixel input, `v = lerp(v, op(v), m(x)·mix)`. Unified-memory machines (APUs) collapse the transfer cost, so the cost model must take bus bandwidth as a measured parameter (design-for-hardware-range rule).
  - **Prior art.** Blender's GPU compositor fuses pixel ops into one shader and uses a ref-counted texture pool, aiming for minimal CPU/GPU differences, not bit-exact. Houdini Copernicus is GPU-resident on OpenCL, and its VRAM exhaustion bugs show budgeting and eviction are needed from day one. darktable has a per-module CPU/GPU cost model and tiles on low VRAM. StarPU's dmda/dmdar does placement by exec+transfer cost with locality preference. Core Image fuses kernels across the graph. Nuke has no graph-level residency as of 17.
  - **Backend lean** (needs the user's confirmation in M83): Vulkan compute with kernels in Slang (Khronos; one source compiles to SPIR-V, CUDA, Metal and C++ for a CPU twin). Reasons: explicit memory, transfer queues, timeline semaphores, headless operation, float controls, external-memory interop with the GL viewer and OFX CUDA plugins. Flame and Houdini moved this way. Rejected: OpenGL (no explicit memory or queues, sync transfers); OpenCL (GEGL driver-stability history, flat vendor investment); CUDA as the core (vendor lock; fine as an optional OFX/ML path); WebGPU (weak precision guarantees); SYCL (heavy toolchain); Qt RHI (no transfer or memory control; keep it for the viewer). CPU/GPU parity is tolerance-based, never bit-exact.

  **New milestone file `PLAN/MILESTONES/M82-render-cost-profiling.md`:**

  # M82 - Render Cost Profiling

  Measure where render time actually goes on real comps, and what host↔GPU transfers cost on real hardware, before committing to any GPU architecture. The output is a go/no-go and a shape for M83 - GPU Compute Backend Spike and M84 - GPU Placement And Residency: do expensive nodes sit close enough to form resident regions, or are they isolated, in which case a few heavy nodes with async transfers is enough?

  ## Phase 82.1: Instrumentation

  - [ ] M82.P1.T1 — Record per-task render cost in the task-graph scheduler
    - files: `Engine/RenderScheduler.cpp`, `Engine/RenderScheduler.h`, a new `Tests/RenderProfile_Test.cpp`
    - approach: In `RenderScheduler::executeTask`, when a profiling switch is on (env var `NATRON_RENDER_PROFILE=<path>`; off by default, zero cost when off), record per task: plugin ID, node script name, time, view, mipmap level, RoI pixel count, components and bit depth, input-dependency task IDs, `estimatedBytes`, wall time of `renderRoI`, and whether the node is a pure point op (native `PixelKernel` effect) or reports GL support. Append records to a JSON-lines file per process, buffered and mutex-guarded so pool threads don't contend on I/O.
    - verify: a gtest renders a small Read → Grade → Blur → Merge graph with the variable set and checks one record per task with a non-zero time and correct dependency edges.
    - size: M
  - [ ] M82.P1.T2 — Region analysis report script
    - files: a new `tools/bench/render-profile-report.py`
    - approach: Read the JSON-lines output. Aggregate time per plugin. Mark nodes as heavy (above a configurable ms/Mpx threshold) or cheap. Find connected regions of "GPU-candidate" nodes (heavy nodes plus the cheap nodes linking them). For each region, compare its CPU time with its boundary bytes ÷ a bandwidth parameter (default 25 GB/s; unified memory as a second scenario). Print a per-comp summary: share of frame time in heavy nodes, number and size of regions, estimated net GPU gain.
    - verify: runs on M82.P1.T1's test output and on a synthetic file with a known answer.
    - size: M

  ## Phase 82.2: Measurement

  - [ ] M82.P2.T1 — Host↔GPU transfer microbenchmark
    - files: spike code under `build/` only; results into a `PLAN/DESIGN/` note
    - approach: Measure upload and download of a UHD RGBA float frame (and half, and 8-bit) through the existing GL PBO path, and through plain Vulkan with pinned staging if that's quick to stand up. Runs on the host GPU (the container has no usable GPU under Xvfb). Record PCIe generation and width.
    - verify: a design note with measured GB/s per direction and per path.
    - size: M
  - [ ] M82.P2.T2 — Profile representative comps
    - files: a `PLAN/DESIGN/` report
    - approach: Profile at least three comps with M82.P1.T1 and M82.P1.T2: a keying-and-grade comp, a CG multi-pass merge comp, and a defocus/retime-heavy comp. Use user-supplied comps if available, otherwise build plausible ones. The dev host is 4-core, so state the core count next to every number and prefer a faster host if one is available. Feed in M82.P2.T1's bandwidth.
    - verify: the user reviews the report and gives a go/no-go plus scope for M83 - GPU Compute Backend Spike.
    - size: M

  **Verification gate:** `ctest` green including `RenderProfile_Test`; profiling off by default with no measurable overhead; the transfer note and comp report exist in `PLAN/DESIGN/`; the user has given a go/no-go on M83 - GPU Compute Backend Spike.

  **New milestone file `PLAN/MILESTONES/M83-gpu-compute-backend-spike.md`:**

  # M83 - GPU Compute Backend Spike

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Pick the engine-wide GPU compute backend once, for native nodes, the task graph and M81 - Raw GPU Kernels. Leading candidate: Vulkan compute (VMA for allocation, a dedicated transfer queue, timeline semaphores) with kernels in Slang, whose C++ target doubles as the CPU twin for point and gather kernels. Prototype headless device init (no X, no EGL), one point kernel and one neighbourhood kernel (a separable blur), upload and download through pinned staging with overlap, and zero-copy hand-off to the Qt GL viewer through external memory. Compare against GL 4.3 compute on the existing context and OpenCL 3.0 on the same kernels. Measure, check CPU/GPU tolerance, and record build and deployment cost.

  Blocked on: M82 - Render Cost Profiling's go/no-go, and the user confirming the backend direction.

  Acceptance sketch:
  - A project-wide decision naming the backend, kernel language and CPU-fallback strategy, with measured numbers.
  - The prototype runs headless in the container (llvmpipe or lavapipe acceptable for correctness) and on the host GPU for speed.
  - CPU/GPU tolerance for the prototype kernels stated and met.

  **New milestone file `PLAN/MILESTONES/M84-gpu-placement-and-residency.md`:**

  # M84 - GPU Placement And Residency

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Teach the task-graph scheduler to place work on CPU or GPU, and keep GPU results resident between tasks. Hook points:
  - A placement pass after `RenderScheduler::buildGraph` (`Engine/RenderScheduler.cpp:238-366`), adding `device` and `regionId` fields to `FrameGraph::Task` (`RenderScheduler.h:59-98`). This moves the CPU/GPU decision out of `renderRoI` (`EffectInstanceRenderRoI.cpp:694-786`).
  - A StarPU-style cost model: measured per-plugin CPU and GPU cost (from M82's instrumentation) plus calibrated bus bandwidth. Accept a connected region only when the CPU time it saves exceeds its boundary transfer cost.
  - Per-task output storage instead of the hard-coded RAM (`RenderScheduler.cpp:711`).
  - A `FrameStore` that can hold device images, so multi-consumer nodes can stay on the GPU (drops the visits>1 refusal).
  - Run each resident region as one super-task, so context or queue affinity isn't a problem.
  - A ref-counted device memory pool with a VRAM budget and LRU eviction to RAM, with GPU bytes counted separately in admission (`admitGatedLocked`, `bytesInFlightLocked`).
  - Pinned staging buffers with async upload and readback overlapping compute.
  - Full-frame or region-of-interest GPU units (fine tiling stays a CPU concern).
  - GPU tasks get a thread budget of 1.
  - Graceful fallback to CPU on out-of-memory or device loss.
  - Fix the stale GL statement in the M63 decision record.

  Blocked on: M83 - GPU Compute Backend Spike's backend decision.

  Acceptance sketch:
  - A chain of GPU-capable nodes renders with transfers only at the region boundary (asserted by transfer counters in a test).
  - Multi-consumer GPU nodes render once.
  - The VRAM budget is respected under a large comp, with eviction and no failure.
  - Placement adapts when the bandwidth parameter models unified memory.
  - No regression in CPU-only renders.

  **New milestone file `PLAN/MILESTONES/M85-native-gpu-kernels.md`:**

  # M85 - Native GPU Kernels

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Give native nodes GPU implementations on the M83 backend.
  - Heavy nodes first, ranked by M82's profile (likely Blur, Defocus/Convolve, Transform with filtering, and the OCIO and colour pipeline).
  - Then the cheap point ops (Grade, ColorCorrect, Merge, Shuffle, Copy, Premult/Unpremult, Clamp and the like) — mainly so they don't break a resident region.
  - Add a backend-neutral kernel description next to `makeKernel()` (`Engine/Nodes/Image/PixelKernel.h`). Mask, mix, process-channels and premult wrapping become per-pixel parts of each kernel.
  - Native nodes report GPU support (today `EffectInstance::supportsOpenGLRender` defaults to None for them, `EffectInstance.h:1772`).

  Blocked on: M84 - GPU Placement And Residency, and the ranked node list from M82 - Render Cost Profiling.

  Acceptance sketch:
  - Each ported node matches its CPU output within a stated tolerance on fixtures.
  - A measured end-to-end speed-up on the M82 comps on a real GPU.
  - Silent CPU fallback with no device.

  **New milestone file `PLAN/MILESTONES/M86-ofx-gpu-suites.md`:**

  # M86 - OFX 1.5 GPU Suites

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Host the OpenFX 1.5 GPU render suites, so third-party GPU plugins can join a resident region instead of forcing a round trip:
  - CUDA streams through Vulkan external-memory interop; OpenCL if the backend allows.
  - Pass the host queue or stream to the plugin, and don't wait for the plugin's work to finish.
  - Keep the existing OpenGL suite working through GL/Vulkan interop for Shadertoy and the OCIO plugins.

  Blocked on: M84 - GPU Placement And Residency.

  Acceptance sketch:
  - An OFX 1.5 CUDA or OpenCL sample plugin renders inside a resident region with no host round trip.
  - The existing GL plugins still render.

  **New milestone file `PLAN/MILESTONES/M87-point-op-kernel-fusion.md`:**

  # M87 - Point-Op Kernel Fusion

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Fuse connected regions of pixel-wise ops (including merges and masked ops with different masks: each mask becomes an extra per-pixel input, `v = lerp(v, op_i(v), m_i(x)·mix_i)`, and stages are skipped where their mask is 0) into one generated kernel on GPU, and optionally as a CPU row-strip pass. A deliberately low-priority optimization: point ops are memory-bound and cheap, so the gain is mostly memory traffic and `FrameStore` footprint.

  Blocked on: evidence from M82 - Render Cost Profiling or the M85 - Native GPU Kernels profiles that long point-op regions are common and fusion would measurably help.

  Acceptance sketch:
  - Fused output matches unfused output within tolerance.
  - A measured gain on a real comp justifies the complexity.

### 2026-10-09T22:08:44-04:00 — change-request
- refs: M80.P6.T1, M81
- Align the raw-kernel backend choice with the engine-wide GPU backend instead of choosing one only for raw kernels. Edit the approach of M80.P6.T1 (M80 - Native RAW Support): defer the decision to M83 - GPU Compute Backend Spike, or include Vulkan compute + Slang as a third option alongside OpenCL and GL shaders. The engine should end up with one GPU backend, not an OpenCL raw path plus a separate engine backend. darktable's OpenCL kernels are C-like and should port to Slang with modest effort. Add M83 - GPU Compute Backend Spike to M81 - Raw GPU Kernels's `Blocked on:` line. Ideally M81 also lands after M84 - GPU Placement And Residency, so raw kernels feed a resident region (demosaic → WB → highlights → denoise is exactly a chain that should stay on the device).

### 2026-10-09T22:08:44-04:00 — info
- refs: M68
- M68 - Headless GL EGL may shrink if M83 - GPU Compute Backend Spike picks Vulkan, which is headless by design. Headless GL is still needed for the existing OFX GL plugins (Shadertoy, OCIO) in background renders and for GL test coverage in CI, so don't cancel it. Re-scope it when M83 lands. No plan edit needed now; still deferred pending the user's go-ahead.

### 2026-10-10T15:36:00-04:00 — change-request
- refs: M91, M84, M85, M86, M87, M81
- Add a new milestone, M91 - NVIDIA GPU Validation (user-requested 2026-10-10). Every GPU milestone so far is verified only on lavapipe and RADV (RX 7900 XTX); the user wants proof that the Vulkan + Slang path also works on an NVIDIA host. It needs physical access to an NVIDIA machine, so it is a stub until one is available. Board row (append after M90; ID M91 checked free against origin/plan at bccb8f659 per the two-hosts-share-the-plan-branch decision): `| M91 | NVIDIA GPU Validation | todo | M84 | [M91-nvidia-gpu-validation.md](PLAN/MILESTONES/M91-nvidia-gpu-validation.md) |`. Not eligible for any lane on either current host. Full file content for `PLAN/MILESTONES/M91-nvidia-gpu-validation.md`:

  ```markdown
  # M91 - NVIDIA GPU Validation

  > Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

  Confirm the GPU work (Vulkan 1.3 compute, offline Slang→SPIR-V, host-import, the Vulkan→GL viewer hand-off) runs correctly on an NVIDIA GPU with the proprietary driver, not only on RADV and lavapipe. Run the existing GPU test and bench tooling on an NVIDIA host, fix any vendor-specific failures (or file them as follow-ups if large), and record the numbers next to the RX 7900 XTX ones. Scope is whatever GPU milestones (M84 onward, plus M81/M85–M87 if done by then) have landed when an NVIDIA host is available.

  Blocked on: access to an NVIDIA host (none is available yet — the user will say when), and M84 - GPU Placement And Residency being `done` so there is a promoted `Engine/Gpu/` and `GpuTests` binary to validate.

  Acceptance sketch:
  - `GpuTests` and a full engine ctest with `NATRON_GPU=force` are green on the NVIDIA device, including semaphore and GL interop tests.
  - GPU-vs-CPU kernels (Grade, FIR Blur, and any later native kernels) agree within the same tolerances as on RADV.
  - Auto mode selects the NVIDIA device; host-import (`VK_EXT_external_memory_host`) and the PBO/semaphore viewer path work under NVIDIA's GL, or fall back cleanly to hostsync/readback with the reason logged.
  - `GpuBench` / transfer-bench numbers recorded in the GPU design note alongside the RADV figures; any vendor-specific workaround recorded as a decision.
  ```

### 2026-10-10T23:40:13-04:00 — change-request
- refs: M94
- Add a new milestone, M94 - Delete Unused Maintainer Skill, with board row
  `| M94 | Delete Unused Maintainer Skill | todo | - | [M94-delete-unused-maintainer-skill.md](PLAN/MILESTONES/M94-delete-unused-maintainer-skill.md) |`.
  User request: delete `skills/` entirely — they noticed it had gone stale
  again and nothing in the repo actually consumes it. Full milestone-file
  content to write to `PLAN/MILESTONES/M94-delete-unused-maintainer-skill.md`:

  ```markdown
  # M94 - Delete Unused Maintainer Skill

  `skills/natron-maintainer/SKILL.md` is the only file in `skills/`. M12.P1.T1
  (Documentation cruft removal) deliberately fixed this file rather than
  deleting it, on the theory that it's "agent-facing" guidance worth keeping
  accurate. Reassessed at the user's request: nothing in this repo wires it up
  for automatic discovery by an agent session — there is no `CLAUDE.md` and no
  `.claude/` directory, and it did not surface in a live session's
  available-skills listing — so its only path to being read is a human
  manually pointing an agent at it. The user has also found at least one place
  where it has gone stale again. Given no automated consumer and a repeat
  drift problem, delete it rather than maintain it.

  ## Phase 94.1: Remove the directory

  - [ ] M94.P1.T1 — Delete the skills/ directory
    - files: `skills/natron-maintainer/SKILL.md` (and the now-empty `skills/`
      directory)
    - approach: `git rm -r skills/`. Before removing, grep the tracked tree
      for any reference to `skills/natron-maintainer` or `skills/` outside
      `.plan/` and `docs/decisions/` (the decision archive is allowed to cite
      deleted paths per the M12.P2.T1 precedent — it's a dated record, not a
      live reference) and confirm there is none. Record this as a
      milestone-scoped decision: it reverses M12.P1.T1's "fix, don't delete"
      call, because the premise behind keeping it — that it's read as live
      agent instruction — turned out not to hold in this repo (no
      `CLAUDE.md`, no `.claude/`, no auto-discovery).
    - verify: `test ! -e skills` (directory is gone); `git status` clean;
      `grep -rn "skills/natron-maintainer" --exclude-dir=.git --exclude-dir=.plan .`
      returns nothing.
    - size: S

  **Verification gate:** `skills/` no longer exists in the tree; no tracked
  file outside `.plan/` and `docs/decisions/` references
  `skills/natron-maintainer`; a clean build is unaffected (no build input
  depended on it).
  ```

### 2026-10-11T00:00:11-04:00 — change-request
- refs: M85, M86, M68
- User request (2026-10-11): fold the **retirement of the engine-side GL render path** into
  M85 - Native GPU Kernels and M86 - OFX 1.5 GPU Suites, so Vulkan compute becomes the only
  engine GPU path and GL survives in the engine solely as a context for OFX OpenGL-suite
  plugins. The viewer (`Gui/ViewerGL*`, overlays) and the 2D GL widgets (curve editor, dope
  sheet, timeline, histogram) are **out of scope** and stay on GL.

  Scouted on `main`-based code: native nodes never render through GL
  (`EffectInstance::supportsOpenGLRender` defaults to None). The engine GL path exists for the
  OFX OpenGL render suite (`OfxHost.cpp:307`) and comprises:
  - GL texture image storage and its host-side GL wrappers: `eStorageModeGLTex` paths in
    `Engine/Image.cpp`, `ImageConvert.cpp`, `ImageCopyChannels.cpp`, `ImageMaskMix.cpp`,
    `ImagePremult.cpp`, with `Engine/GLShader.*` and `Engine/Texture.cpp`.
  - Per-render-thread contexts: `Engine/OSGLContext*` (x11, wayland, xdg), `GPUContextPool.*`,
    and the plumbing in `RenderScheduler.cpp`, `FrameRenderContext.cpp`,
    `ParallelRenderArgs.*`, `EffectInstanceRenderRoI.cpp`, `EffectInstanceRenderDeep.cpp`,
    `OfxClipInstance.cpp`, `CacheEntry.h`.
  - Startup GL probing and settings: `AppManager*.cpp` renderer probe, `Settings.cpp`
    `_enableOpenGL` / `_availableOpenGLRenderers`, `Gui/AboutWindow.cpp`.
  - Tests: `Tests/GLScheduler_Test.cpp`, `Tests/OSGLContext_Test.cpp`.
  (`RotoPaint*`, `TrackerNode*` GL calls are overlay drawing — viewer side, out of scope.)

  **Edit `PLAN/MILESTONES/M85-native-gpu-kernels.md`** (stays a stub; still blocked on
  M84 - GPU Placement And Residency). Append to the intent bullets:

  ```markdown
  - **Retire the engine GL render path for host work** (2026-10-11, user). The host's GL
    wrappers around plugin renders — mask/mix, copy-channels, premult and storage conversion
    on `eStorageModeGLTex` images (`ImageMaskMix.cpp`, `ImageCopyChannels.cpp`,
    `ImagePremult.cpp`, `ImageConvert.cpp`, GL paths in `Image.cpp`, `GLShader.*`) — are
    replaced by the backend-neutral kernels above running on Vulkan. GPU device selection
    and the on/off switch in `Settings.cpp` move from the GL renderer list to the Vulkan
    device list. After M85 nothing native or host-side renders through GL; the remaining GL
    engine code serves only the OFX OpenGL suite and is narrowed by M86.
  ```

  and append to its acceptance sketch:

  ```markdown
  - No host-side image operation renders through GL: the `eStorageModeGLTex` mask/mix,
    copy-channels, premult and convert paths are gone or reduced to the M86 interop boundary.
  - The Preferences GPU setting lists Vulkan devices, not GL renderers.
  ```

  **Edit `PLAN/MILESTONES/M86-ofx-gpu-suites.md`** (stays a stub; still blocked on
  M84 - GPU Placement And Residency). Replace the bullet "Keep the existing OpenGL suite
  working through GL/Vulkan interop for Shadertoy and the OCIO plugins." with:

  ```markdown
  - Keep the existing OpenGL suite working for Shadertoy and the OCIO plugins, as the **only
    remaining engine GL path** (2026-10-11, user). A GL-suite plugin's inputs and outputs
    cross a GL/Vulkan interop boundary at the plugin call (exported Vulkan memory imported
    as GL textures or PBOs, ordered by shared semaphores with the hostsync fallback from
    M83). Shrink `OSGLContext*` and `GPUContextPool` to lazily created contexts used only for
    those plugins — none are created when a project has no GL-suite plugin — and delete the
    GL scheduling plumbing in `RenderScheduler.cpp`, `FrameRenderContext.cpp` and
    `ParallelRenderArgs.*` that no longer has a consumer. Rework or retire
    `Tests/GLScheduler_Test.cpp` and `Tests/OSGLContext_Test.cpp` to match. M68 - Headless
    GL EGL then reduces to giving this narrowed context an EGL surfaceless backend.
  ```

  and append to its acceptance sketch:

  ```markdown
  - With no GL-suite plugin in the project, a render creates no GL context.
  - GL is used in the engine only at the OFX OpenGL-suite boundary; everything else on the
    GPU runs on Vulkan.
  ```

  **Record a project-wide decision** `PLAN/DECISIONS/2026-10-11-retire-engine-gl-render-path.md`
  (+ INDEX line): Vulkan compute is the only engine GPU path; the engine's GL render path is
  retired across M85 - Native GPU Kernels (host wrappers, settings) and M86 - OFX 1.5 GPU
  Suites (contexts narrowed to the OFX OpenGL suite behind interop). The viewer and the 2D GL
  widgets stay on GL: OFX overlay interacts require a GL context, Qt 6 allows one graphics
  API per top-level window so the viewer can't move alone, and the M83 interop hand-off
  already costs 0.79 ms at UHD. A later viewer modernisation should go through `QRhiWidget`
  on its OpenGL backend first; the 2D widgets, if reworked, go to `QPainter`, not Vulkan.

### 2026-10-11T00:04:16-04:00 — change-request
- refs: M69
- User request (2026-10-11): in **M69 - Minor Cleanup** (stays a stub), add a new "Panels:" intent
  group, placed after "Node graph interaction" and before "Roto". Right-clicking the empty space
  beside a panel's tabs must bring up the same pane menu as the panel management icon (the
  top-left corner button of a pane). Append this to the stub body:

  ```markdown
  Panels:
  - **Right-click the empty header space for the pane menu.** Right-clicking the empty space to
    the right of a pane's tabs opens the same menu as the panel management icon at the pane's
    top-left (split vertical/horizontal, float, close, hide toolbar/tabs header, "... here"
    entries, set as anchor). Scouted 2026-10-11: that menu is `TabWidget::createMenu()`
    (`Gui/TabWidget.cpp`), connected to `leftCornerButton`'s `clicked()` and shown with
    `menu.exec(leftCornerButton->mapToGlobal(QPoint(0, 0)))`. The empty space is the stretch in
    the pane's header row, which belongs to `TabWidgetHeader` (`Gui/TabWidget.h`), a bare
    `QWidget` that handles only `leaveEvent`. Approach: give `TabWidgetHeader` a
    `contextMenuEvent` (or a `mousePressEvent` for the right button) that emits a signal
    connected to a `TabWidget` slot; refactor `createMenu()` to take the global position to
    open at (the icon passes its own corner, the header passes the cursor) so the menu
    opens at the right place the first time. Keep the icon's behaviour unchanged, and don't
    swallow right-clicks on the tabs themselves (`TabBar`) or the float/close buttons. Fold in
    the "Tab menu position" fix from the node-graph group if the same positioning bug turns out
    to be shared.
  ```

  and append to its acceptance sketch:

  ```markdown
  - Right-clicking the empty space beside a pane's tabs opens the same menu as the panel
    management icon, at the cursor.
  ```

### 2026-10-11T00:40:00-04:00 — info
- refs: none
- **Two new related features from the user (2026-10-11), filed as five milestones below:** live multi-user sessions, where one Natron hosts a project and others join and see each other's edits live, and node-graph diff with three-way template merge (a supervisor publishes template v1, an artist modifies it, the supervisor publishes v2, and the artist merges). Both rest on one shared foundation: **M95 - Graph Edit Operations**, which adds a persistent node ID plus plain-data graph-edit operations that can be recorded, serialized, sent and applied. The order is M16 - Project Format Redesign → M95 - Graph Edit Operations → (M96 - Graph Diff And Merge → M97 - Merge Resolution UI) in parallel with (M98 - Live Session Core → M99 - Live Session Presence And Undo). Suggested board rows, in this order after M16 - Project Format Redesign:
  ```
  | M95 | Graph Edit Operations | todo | M16 | [M95-graph-edit-operations.md](PLAN/MILESTONES/M95-graph-edit-operations.md) |
  | M96 | Graph Diff And Merge | todo | M95 | [M96-graph-diff-and-merge.md](PLAN/MILESTONES/M96-graph-diff-and-merge.md) |
  | M97 | Merge Resolution UI | todo | M96 | [M97-merge-resolution-ui.md](PLAN/MILESTONES/M97-merge-resolution-ui.md) |
  | M98 | Live Session Core | todo | M95 | [M98-live-session-core.md](PLAN/MILESTONES/M98-live-session-core.md) |
  | M99 | Live Session Presence And Undo | todo | M98 | [M99-live-session-presence-and-undo.md](PLAN/MILESTONES/M99-live-session-presence-and-undo.md) |
  ```
  Suggested `# Context and constraints` bullet: "**M95–M99 (live sessions + graph merge)** added 2026-10-11 via the inbox (user-requested). M95 - Graph Edit Operations is the shared foundation (persistent node ID, plain-data edit operations). M96 - Graph Diff And Merge and M97 - Merge Resolution UI deliver three-way template merge. M98 - Live Session Core and M99 - Live Session Presence And Undo deliver live multi-user editing. All of it runs after M16 - Project Format Redesign. See `DECISIONS/2026-10-11-live-sessions-and-graph-merge.md`."
  Suggested project-wide decision `DECISIONS/2026-10-11-live-sessions-and-graph-merge.md` (+ INDEX line). The user decided on 2026-10-11:
  - **Live sessions are host-authoritative.** The host orders every edit and rebroadcasts it. On the same knob, the last writer wins. Peers stay connected (LAN/VPN) and there is no offline editing. CRDTs and node locking were rejected.
  - **Live-session v1 scope:** graph, knob and position sync, plus presence (each user's selection and open panels, in their colour), per-user undo, and follow-a-user's-viewer. **No session password in v1**, so the host dialog warns that anyone who can reach the port can join.
  - **Each peer renders locally with its own cache.** Only the script is shared, and media paths are assumed to resolve on every machine (shared storage).
  - **Merge is available in the app and on the CLI.** The CLI (`natron-merge`) auto-applies clean changes, records conflicts in the file, and doubles as a git merge driver and textconv. The in-app merge view highlights conflicted nodes and resolves per knob.
  - **A template is "just a bunch of nodes in a script" that lives in a file on disk.** Merge therefore works on the set of nodes that came from template T, not on a whole project or a Group.
  - **Node identity:** persist a node ID, since live sessions need one, but treat it as one matching signal among several. The user's example: an artist deletes a Grade and makes a new one in the same place, which "in a sense" is the same node. The merge's matching policy is decided by a scored spike, M96.P1, and **the user makes the call**.
  - **Node-specific merge resolution:** nodes and knob kinds can carry their own merge handlers, for example per-keyframe curve merge, per-item roto merge and per-track tracker merge. Prior art: the user's `charlesangus/nuke_diff`, whose lessons are recorded in M96's notes.

### 2026-10-11T00:41:00-04:00 — change-request
- refs: M95 (new milestone)
- New milestone **M95 - Graph Edit Operations**, file `PLAN/MILESTONES/M95-graph-edit-operations.md`, board row `| M95 | Graph Edit Operations | todo | M16 | [M95-graph-edit-operations.md](PLAN/MILESTONES/M95-graph-edit-operations.md) |`. Full file content:

````markdown
# M95 - Graph Edit Operations

Shared foundation for M96 - Graph Diff And Merge and M98 - Live Session Core. Every node gets a
**persistent ID**, and graph edits become **plain-data operations**. An operation can be
recorded from the live graph, serialized, sent, applied to a live graph, applied to a plain
snapshot, or computed as the difference between two snapshots. Depends on M16 - Project Format
Redesign (plain `Serialization/` structs and YAML encoders). No user-facing feature ships here.

Scouting facts (2026-10-11, `main` at `10e0accd5`; re-check line numbers, since M93 and M16 move them):
- There is no node ID. `Node::getHashValue()` (`Engine/Node.h:232`) is a content hash for
  caching. Script names are assigned in `Node::initNodeScriptName` (`Engine/NodeName.cpp:53`).
  A rename (`Node::setScriptName`, `NodeName.cpp:427`) uniquifies the name within its group via
  `NodeGroup::checkNodeName` (`Engine/NodeGroup.h:117`).
- Edits have no single hook. The chokepoints are:
  - knob changes: `KnobHelper::evaluateValueChange[Internal]` (`Engine/Knob.cpp:1870,1827`), whose `reason` argument separates user, plugin and internal edits;
  - create: `AppInstance::createNode[Internal]` (`Engine/AppInstance.cpp:1393,1108`);
  - delete: `Node::destroyNode` (`Engine/NodeMain.cpp:905`). The GUI deletes through `Node::deactivate`/`activate` (`Engine/Node.cpp:3549,3786`) so that undo works;
  - inputs: `Node::connectInput`/`replaceInput`/`disconnectInput` (`Engine/NodeInputs.cpp:765,956,1105,1255`);
  - position: `Node::setPosition` (`Engine/Node.cpp:7719`);
  - rename and label: the `Node::scriptNameChanged`/`labelChanged` signals (`Node.h:1837-1838`).
- Undo stacks are GUI-only (`QUndoStack`, `Gui/NodeGraph.h:288`). The commands hold `NodeGuiPtr` and are not serializable. `Engine/UndoCommand.h` is a thin Engine-side interface.

## Design (applies to every task)

- **Node ID:** 128-bit random, stored as 32 lowercase hex characters (`QUuid::createUuid()`, no
  braces or dashes). It lives on `Node`, in plain `NodeSerialization::id`, and as the YAML
  `id:` key. It is minted at creation and kept on load. Paste, duplicate and import mint fresh
  IDs. A duplicate ID found while loading is reminted, with a load warning (M16's
  load-warnings list). Undo of a delete restores the original ID, because the GUI delete is a
  deactivate.
- **Operations** (`Serialization/Ops/GraphOp.h`): a `std::variant` of
  - `CreateNode{parentId|root, NodeSerialization (children included)}`
  - `DeleteNode{id}`
  - `SetKnob{nodeId|project, knob script name, KnobSerialization of the whole knob, or "default"}`
  - `SetInput{nodeId, input label, sourceId|none}`
  - `Rename{nodeId, scriptName}`
  - `SetNodeUi{nodeId, position?, size?, colour?}`
  - `SetUserKnobs{nodeId, user knob definitions + pages}`
  - `SetRotoData{nodeId, RotoContextSerialization}`
  - `SetTrackerData{nodeId, TrackerContextSerialization}`
  - `SetProjectData{formats?, layer registry?}`

  Operations name nodes **by ID, never by script name**, so a concurrent rename cannot misdirect
  them. Roto and tracker edits are whole-data operations in v1.
- **Encoding:** YAML through the M16 encoders. One operation is a map with an `op:` key, and a
  batch is a sequence. The same encoding is used on the wire (M98 - Live Session Core) and in
  diff output (M96 - Graph Diff And Merge).
- **Apply on snapshots:** `applyOps(ProjectSerialization&, ops)` is a pure function in
  `Serialization/`. **Apply on the live graph** is in `Engine/GraphOpApply`, which runs under a
  `GraphEditRecorder::Suppress` guard so applied operations are never re-recorded. Each
  operation returns applied, target-missing or invalid, and never crashes. Undo wrapping is the
  caller's job (M97 - Merge Resolution UI wants it; M98 - Live Session Core must not have it).
- **Two-way snapshot diff:** `diffSnapshots(a, b)` returns the operations that turn `a` into
  `b`, matching nodes by ID. Nodes are created parent-first, with every `SetInput` in a later
  pass so creation order never matters. Output order is deterministic.
- **Recording:** `Engine/GraphEditRecorder` observes the chokepoints and emits operations for
  every edit that changes serialized state: user, plugin and Python edits alike. It skips
  loading, suppressed applies and time-only refreshes. Knob edits are coalesced per knob per
  event-loop turn into one `SetKnob`. With no subscriber it costs one branch per hook.
- **Invariant**, the milestone's core test: snapshot S0, run an edit session while recording,
  snapshot S1. Replaying the recorded operations on a fresh load of S0 gives a `project.yaml`
  byte-identical to S1. `applyOps(S0, diffSnapshots(S0, S1))` equals S1.

## Phase 95.1: Persistent node identity

- [ ] M95.P1.T1 — Persist a node ID
  - files: `Engine/Node.h`, `Engine/NodeName.cpp`, `Serialization/NodeSerialization.h`, `Serialization/Yaml/NodeYaml.cpp`, new `Tests/NodeId_Test.cpp`
  - approach: implement the ID rules in **Design**: mint at creation, keep on load, mint fresh on paste, duplicate and import, and remint a duplicate on load with a warning. Extend `Node::toSerialization` and `fromSerialization` (M93's conversions) to carry it. YAML emits `id:` first in each node map.
  - verify: compiled at P1.T3. gtest: a new node has a 32-hex ID and two nodes differ; save → load keeps IDs; copy/paste and duplicate get fresh IDs; deleting a node and undoing the delete keeps its ID; a hand-edited bundle with a duplicate ID loads with one reminted ID and one warning.
  - size: M

- [ ] M95.P1.T2 — Find nodes by ID, in Engine and Python
  - files: `Engine/NodeGroup.h`, `Engine/NodeGroup.cpp`, `Engine/Project.h`, `Engine/PyNode.cpp`, `Engine/PyAppInstance.cpp`
  - approach: a per-project `id → NodeWPtr` index, maintained on create, destroy, activate and deactivate, and when a node moves into or out of a group. Add `Project::findNodeById(id)`, Python `Effect.getNodeId()` and `app.getNodeById(id)`, with the PySide/Shiboken typesystem updated if the bindings are generated.
  - verify: compiled at P1.T3. A gtest covers lookup after create, after a move into a group, and after delete (empty result) followed by undo. A Python test round-trips `app.getNodeById(n.getNodeId())`.
  - size: M

- [ ] M95.P1.T3 — Phase 95.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 95.2: Operation model

- [ ] M95.P2.T1 — Operation data types and YAML encoding
  - files: new `Serialization/Ops/GraphOp.h`, new `Serialization/Ops/GraphOp.cpp`, new `Serialization/Yaml/GraphOpYaml.cpp`, `Serialization/CMakeLists.txt`, new `Tests/Serialization/GraphOpYaml_Test.cpp`
  - approach: implement the variant in **Design**. Encoding reuses the M16 node, knob, roto and tracker encoders. Unknown `op:` values decode to an error result, not an exception that aborts the whole batch.
  - verify: compiled at P2.T5. Every operation kind round-trips encode → decode with field equality; a batch of mixed operations round-trips; an unknown op gives an error entry and the rest still decode.
  - size: M

- [ ] M95.P2.T2 — Apply operations to a plain snapshot
  - files: new `Serialization/Ops/ApplyOps.h`, new `Serialization/Ops/ApplyOps.cpp`, new `Tests/Serialization/ApplyOps_Test.cpp`
  - approach: `applyOps(ProjectSerialization&, const std::vector<GraphOp>&) → std::vector<OpResult>`. Find nodes by ID across nested `children`. Deleting a group removes its subtree. `SetInput` to a missing source gives target-missing.
  - verify: compiled at P2.T5. Each operation kind is applied to a built snapshot and checked; a missing target returns the right result and leaves the snapshot unchanged.
  - size: M

- [ ] M95.P2.T3 — Two-way snapshot diff
  - files: new `Serialization/Ops/SnapshotDiff.h`, new `Serialization/Ops/SnapshotDiff.cpp`, new `Tests/Serialization/SnapshotDiff_Test.cpp`
  - approach: implement `diffSnapshots` from **Design**: deletes, then creates parent-first, then knob, rename, UI and user-knob changes, then every `SetInput` last. Compare knobs by `KnobSerialization` equality, so default-valued knobs that are absent on both sides are equal.
  - verify: compiled at P2.T5. For 10 hand-built pairs (add, delete, rename, rewire, animate, nested group add and delete, roto change), `applyOps(a, diffSnapshots(a, b)) == b`, and diffing identical snapshots gives no operations.
  - size: M

- [ ] M95.P2.T4 — Apply operations to the live graph
  - files: new `Engine/GraphOpApply.h`, new `Engine/GraphOpApply.cpp`, `Engine/CMakeLists.txt`, new `Tests/GraphOpApply_Test.cpp`
  - approach: map each operation onto the existing Engine paths:
    - create through `createNode` with a serialization, keeping its ID;
    - delete through `destroyNode`;
    - knobs through M93's `fromSerialization`;
    - inputs through `connectInput`/`disconnectInput`, looked up by input label;
    - rename through `setScriptName`;
    - UI through `Node::setPosition` and the size and colour setters;
    - roto and tracker through their restore paths.

    Run the batch under `GraphEditRecorder::Suppress` (a stub until P3.T1 if needed). Return per-operation results.
  - verify: compiled at P2.T5. A gtest applies every operation kind to a live project and checks state through `toSerialization`; a missing target returns target-missing and does not crash.
  - size: L

- [ ] M95.P2.T5 — Phase 95.2 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 95.3: Recording edits

- [ ] M95.P3.T1 — Recorder core and knob-edit recording
  - files: new `Engine/GraphEditRecorder.h`, new `Engine/GraphEditRecorder.cpp`, `Engine/Knob.cpp`, `Engine/CMakeLists.txt`, new `Tests/GraphEditRecorder_Test.cpp`
  - approach: a per-project recorder with subscribe and unsubscribe, the `Suppress` RAII guard, and a coalescing buffer flushed at the end of the event-loop turn (or explicitly in tests). Hook value, keyframe, expression and default-reset changes in `evaluateValueChange` and the curve and expression setters. Each emits one `SetKnob` with the knob's whole serialization. Skip anything during project load or under `Suppress`.
  - verify: compiled at P3.T6. gtest: setting a value, a keyframe, an expression and a reset each emit one coalesced `SetKnob`; 100 sets in one turn emit one; edits during load or under `Suppress` emit nothing.
  - size: M

- [ ] M95.P3.T2 — Record node creation and deletion
  - files: `Engine/GraphEditRecorder.cpp`, `Engine/AppInstance.cpp`, `Engine/NodeMain.cpp`, `Engine/Node.cpp` (`activate`/`deactivate`)
  - approach: create emits `CreateNode` with the node's full serialization, captured after its knobs are initialised. `destroyNode` and `deactivate` emit `DeleteNode`, and `activate` emits `CreateNode`, so undo of a delete re-creates the node with the same ID. Compound GUI actions such as group-from-selection and inline-group come out as their primitive operations.
  - verify: compiled at P3.T6. gtests in `Tests/GraphEditRecorder_Test.cpp` cover create, delete, delete followed by reactivate (same ID), and group-from-selection.
  - size: M

- [ ] M95.P3.T3 — Record input, rename and node UI edits
  - files: `Engine/GraphEditRecorder.cpp`, `Engine/NodeInputs.cpp`, `Engine/NodeName.cpp`, `Engine/Node.cpp` (`setPosition`, size and colour setters)
  - approach: `connectInput`, `replaceInput` and `disconnectInput` emit `SetInput`, keyed by input label. `setScriptName` emits `Rename`, and position, size and colour emit `SetNodeUi`, coalesced per turn so a node drag sends one operation per node.
  - verify: compiled at P3.T6. gtests in `Tests/GraphEditRecorder_Test.cpp` cover each kind, including a 50-step drag that emits one `SetNodeUi`.
  - size: M

- [ ] M95.P3.T4 — Record roto, tracker, user-knob and project-level edits
  - files: `Engine/GraphEditRecorder.cpp`, `Engine/RotoContext.cpp`, `Engine/TrackerContext.cpp`, `Engine/Knob.cpp` (user-knob add and remove, page changes), `Engine/Project.cpp`
  - approach: any change to a roto item, stroke or shape marks the node dirty, and the turn flush emits one `SetRotoData`. Tracker works the same way with `SetTrackerData`. User-knob and page edits emit `SetUserKnobs`, and project formats or layer-registry changes emit `SetProjectData`.
  - verify: compiled at P3.T6. gtests: drawing a stroke emits one `SetRotoData`, adding a track one `SetTrackerData`, adding a user knob one `SetUserKnobs`.
  - size: M

- [ ] M95.P3.T5 — Record-and-replay invariant test
  - files: new `Tests/GraphEditReplay_Test.cpp`
  - approach: build S0 (a generator, Grade, Merge, RotoPaint, Tracker and a group), subscribe, then run a scripted session of about 30 edits of every kind, including delete with undo-style reactivation, rename, rewire, animation, a group-from-selection and a roto stroke. Snapshot S1. Replay the recorded operations on a fresh load of S0 and compare the encoded `project.yaml` text. Also check `applyOps(S0, diffSnapshots(S0, S1)) == S1`.
  - verify: compiled at P3.T6, where it passes. A deliberately dropped hook, tried locally and reverted, makes it fail.
  - size: M

- [ ] M95.P3.T6 — Phase 95.3 build, full ctest and smoke
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: S

**Verification gate:** full ctest and smoke green, and CI (`format`, `lint-ci`, `build-and-test`)
green. `GraphEditReplay_Test` passes byte-identically, and diff/apply round-trips hold on every
pair. No `Serialization/**` file includes Engine or Gui.

## Decisions

- 2026-10-11 — **User decision:** persist a node ID (live sessions need one), but treat it as one matching signal among several for merge; M96 - Graph Diff And Merge's spike sets the matching policy.
- 2026-10-11 — Operations reference nodes by ID, carry whole-knob serializations, and roto and tracker edits are whole-data operations in v1. This trades bandwidth for simplicity; finer roto operations are a later optimisation if live sessions need them.
````

### 2026-10-11T00:42:00-04:00 — change-request
- refs: M96 (new milestone)
- New milestone **M96 - Graph Diff And Merge**, file `PLAN/MILESTONES/M96-graph-diff-and-merge.md`, board row `| M96 | Graph Diff And Merge | todo | M95 | [M96-graph-diff-and-merge.md](PLAN/MILESTONES/M96-graph-diff-and-merge.md) |`. Full file content:

````markdown
# M96 - Graph Diff And Merge

Three-way merge of node graphs, as an engine and a CLI, with no GUI (that is M97 - Merge
Resolution UI). The workflow: a supervisor publishes a template (a file of nodes), an artist
brings it into a shot script and modifies it, and the supervisor publishes a new version. The
artist merges the upstream changes with their own. Merging runs on plain `ProjectSerialization`
snapshots in `Serialization/Merge/`, so the CLI loads no plugins and starts fast. Depends on
M95 - Graph Edit Operations.

**Phase 96.1 is a spike that stops for the user.** How nodes are matched between versions is
the user's call, made from the spike's scores. Later phases implement the chosen policy behind
a `NodeMatcher` interface that the spike defines.

Prior art, the user's `charlesangus/nuke_diff` (read 2026-10-11). It does a three-way merge of
Nuke Group templates and matches nodes **by name only**, so a rename looks like a delete plus
an add. Its lessons, which this design avoids:
- conflicts are stored in node labels and recovered by scanning label prefixes, which is
  fragile and overwrites the artist's labels;
- "resolve as theirs" swaps in the whole node and drops the artist's non-conflicting
  overrides;
- curve and roto knobs conflict as one opaque string;
- a move on both sides is a full node conflict;
- inline-insert detection follows input 0 only;
- nested groups are not recursed;
- templates work only inside a Group.

## Design (applies to every task)

- **Template provenance.** `Project::importTemplate(path, parentGroup)` loads a template
  bundle and mints fresh node IDs, so the same template can be imported twice. It records a
  template entry: `{key, path, content hash, imported-at}`. A copy of the imported snapshot is
  stored as the bundle member `templates/<key>/base.yaml`, so the merge base survives the
  published file being overwritten. Each imported node carries `origin: {template: <key>,
  node: <template node id>}`. The spike may amend these rules.
- **Merge scope:** the nodes whose `origin` names the template, plus artist-added nodes per
  the scope rule the spike settles (for example, nodes wired strictly between scoped nodes).
  Everything else in the script is untouched.
- **Matching:** a `NodeMatcher` gives base↔mine and base↔theirs correspondences. Policy is
  set at P1.T4.
- **Three-way core.** Node existence follows a table (added, deleted, delete-vs-modify, both
  added). For matched nodes, merge per knob and per dimension, and per input label. Delete
  versus modify is a conflict that keeps the node by default. UI properties (position,
  colour, size) **never conflict**: theirs applies unless mine changed it, in which case mine
  wins.
- **Built-in merge resolution.** A `MergeHandler` registry, keyed by knob kind and by plugin ID:
  - **knob kinds:**
    - curves merge per keyframe and conflict only when both sides changed the same key time differently;
    - strings and text merge line-based (diff3);
    - expressions and links conflict as a whole.
  - **node handlers:**
    - Roto merges per item (by item name, through layers);
    - Tracker merges per track;
    - Group and PyPlug recurse into their children.

    OFX nodes use the generic handlers. Native nodes may register their own.
- **Both sides insert on the same base edge** (any input slot): a structural conflict whose
  default chain order is theirs then mine, recorded as a conflict.
- **Conflicts are data, never labels.** A conflict is `{node id, knob?, dim?, kind, base, mine,
  theirs, handler options}`. The merged snapshot takes **mine** at every conflict, so the
  artist's work is the default. Conflicts are saved as the bundle member
  `merge/conflicts.yaml`, so a conflicted file opens normally and is resolved later in
  M97 - Merge Resolution UI.
- **CLI:** a new `natron-merge` executable that links only `NatronSerialization`:
  - `diff a b` prints a human-readable diff, and `--yaml` prints the operations from M95;
  - `merge base mine theirs -o out` exits 0 when clean and 1 when there are conflicts;
  - `git-driver %O %A %B` follows git's merge-driver contract;
  - `textconv f` prints `project.yaml` for `git diff`.

## Phase 96.1: Identity and provenance spike (stops for the user)

- [ ] M96.P1.T1 — Scenario corpus of template merges
  - files: new `Tests/fixtures/merge-scenarios/build_scenarios.py`, new `Tests/fixtures/merge-scenarios/README.md`, new `Tests/fixtures/merge-scenarios/expected/*.yaml`, generated `Tests/fixtures/merge-scenarios/*/{base,mine,theirs}.ntpz`
  - approach: a NatronRenderer Python script builds at least 12 base/mine/theirs triples:
    - a rename;
    - **an artist deletes a Grade and makes a new one in the same place** (the user's example);
    - delete and recreate as a different type;
    - the artist duplicates a template node;
    - the supervisor replaces a node;
    - both sides insert on the same edge, on input 0 and on a side input;
    - the template is imported twice;
    - template nodes pasted in from another shot;
    - the supervisor wraps nodes into a group;
    - a reconnect;
    - keyframe edits on both sides;
    - roto edits on both sides.

    Each has a hand-written `expected/<name>.yaml` giving the correct correspondences and the expected conflicts.
  - verify: the script builds every triple without error, and `unzip -l` lists `mimetype` first in each. The build is required (NatronRenderer must exist).
  - size: M

- [ ] M96.P1.T2 — Matcher strategies and a scoring harness
  - files: new `Serialization/Merge/NodeMatcher.h`, new `Serialization/Merge/NodeMatcher.cpp`, new `tools/merge/score_matchers.cpp` (or a gtest that prints the table), `Serialization/CMakeLists.txt`
  - approach: define the `NodeMatcher` interface, then implement three strategies:
    - `IdOnly`, which matches on `origin` and ID;
    - `IdPlusAdoption`, which also adopts an unmatched new node when it has the same plugin and the same matched upstream and downstream neighbours on the same input labels as an unmatched removed node. This covers the delete-and-recreate case;
    - `NameBased`, which mirrors nuke_diff.

    The harness scores each strategy against `expected/*.yaml` and prints correct, wrong and missing correspondences per scenario.
  - verify: the harness runs over the whole corpus and prints a table with one row per scenario and one column per strategy.
  - size: L

- [ ] M96.P1.T3 — Provenance and scope prototype, plus spike report
  - files: new `docs/design/graph-merge-identity.md`, the prototype code alongside `Serialization/Merge/NodeMatcher.cpp`
  - approach: prototype the merge-scope rule (provenance-tagged nodes plus artist nodes wired strictly between them, and alternatives) and score it on the corpus. Write the report: the score table from P1.T2, the scope results, the failure cases for each strategy, and a recommendation. Cover the explicit question of whether a deleted-and-recreated node should count as the same node, and when that goes wrong.
  - verify: the report exists, covers every scenario, and makes one recommendation.
  - size: M

- [ ] M96.P1.T4 — User decision on matching policy and scope
  - files: this file's `## Decisions`
  - approach: **stop for the user.** Present the report's table and recommendation, and ask the user to choose the matching policy, the scope rule and any provenance changes. Record the answer as a decision, then re-check P2's briefs against it (PLAN-FORMAT §5a) before starting P2.
  - verify: a user decision is recorded.
  - size: S

## Phase 96.2: Provenance and three-way core

- [ ] M96.P2.T1 — Template provenance in the data model, bundle and Engine
  - files: `Serialization/NodeSerialization.h`, `Serialization/ProjectSerialization.h`, `Serialization/Yaml/NodeYaml.cpp`, `Serialization/Bundle/ProjectBundle.cpp`, `Engine/Project.h/.cpp` (`importTemplate`)
  - approach: the provenance rules from **Design**, as amended at P1.T4. Add the template entry list and the `templates/<key>/base.yaml` members, and the node `origin` field. `importTemplate` mints IDs, sets `origin`, stores the base, and returns the created nodes.
  - verify: compiled at P2.T5. gtest: importing the same template twice gives distinct IDs that share the same `origin.node`; save → load keeps origins and the base member; the base is byte-identical to the imported file's `project.yaml` node subset.
  - size: M

- [ ] M96.P2.T2 — Production matcher and scope
  - files: `Serialization/Merge/NodeMatcher.h/.cpp`, new `Serialization/Merge/MergeScope.h/.cpp`, new `Tests/Serialization/NodeMatcher_Test.cpp`
  - approach: keep only the chosen strategy and scope rule, and delete the losing prototypes and the scoring tool unless the user asked to keep them. Matching is deterministic.
  - verify: compiled at P2.T5. Every corpus scenario's correspondences equal `expected/*.yaml`.
  - size: M

- [ ] M96.P2.T3 — Three-way node, knob and input merge with the handler registry
  - files: new `Serialization/Merge/ThreeWayMerge.h/.cpp`, new `Serialization/Merge/MergeHandler.h/.cpp`, new `Serialization/Merge/MergeConflict.h`, new `Tests/Serialization/ThreeWayMerge_Test.cpp`
  - approach: `merge(base, mine, theirs, scope) → {merged snapshot, conflicts}` for matched nodes: per knob, per dimension, per input label. Default knob values merge as values: a knob absent from the YAML is "default". The generic scalar handler conflicts only on mine ≠ theirs ≠ base. The registry dispatches by plugin ID, then by knob kind. A conflict takes mine.
  - verify: compiled at P2.T5. A table of scalar cases (one side changed, both changed the same way, both changed differently, reset to default on one side) gives the expected merged value and conflict list.
  - size: L

- [ ] M96.P2.T4 — Structural merge
  - files: new `Serialization/Merge/StructuralMerge.cpp`, `Serialization/Merge/ThreeWayMerge.cpp`, new `Tests/Serialization/StructuralMerge_Test.cpp`
  - approach: the node-existence table (added, deleted, delete-vs-modify kept with a conflict, both added). Both inserting on the same base edge on any input slot gives the theirs-then-mine chain plus a structural conflict. Groups recurse. UI properties use the never-conflict rule. Renames follow the matcher, so a rename on one side applies cleanly.
  - verify: compiled at P2.T5. Each structural corpus scenario gives the expected merged graph and conflicts.
  - size: L

- [ ] M96.P2.T5 — Phase 96.2 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 96.3: Built-in handlers and conflict persistence

- [ ] M96.P3.T1 — Curve and text merge handlers
  - files: new `Serialization/Merge/CurveMerge.cpp`, new `Serialization/Merge/TextMerge.cpp`, new `Tests/Serialization/CurveTextMerge_Test.cpp`
  - approach: curves are merged as maps keyed by key time. A key changed or added on one side applies. Keys at the same time changed differently conflict, per key. An animated-versus-static change conflicts as a whole. Text uses a line-based diff3 in-house (no dependency), and overlapping hunks conflict.
  - verify: compiled at P3.T5. Keyframes added on both sides at different times merge clean; the same key changed differently gives one per-key conflict; non-overlapping text edits merge, and overlapping ones conflict.
  - size: M

- [ ] M96.P3.T2 — Roto per-item merge handler
  - files: new `Serialization/Merge/RotoMerge.cpp`, new `Tests/Serialization/RotoMerge_Test.cpp`
  - approach: match items by name within their layer path. Items added or deleted on one side apply. For items changed on both sides, item knobs merge per knob through the generic and curve handlers. Shape control points changed on both sides conflict for the whole shape. Layer order follows theirs, plus mine's additions.
  - verify: compiled at P3.T5. The corpus roto scenario merges as expected; both sides adding different shapes keeps both; both editing one shape's points gives one conflict.
  - size: M

- [ ] M96.P3.T3 — Tracker per-track merge handler
  - files: new `Serialization/Merge/TrackerMerge.cpp`, new `Tests/Serialization/TrackerMerge_Test.cpp`
  - approach: match tracks by script name. Tracks added or deleted on one side apply. A track changed on both sides merges its knob curves per key through the curve handler.
  - verify: compiled at P3.T5. Different tracks added on each side are both kept, and the same track keyed at different frames merges clean.
  - size: M

- [ ] M96.P3.T4 — Persist conflicts in the bundle
  - files: new `Serialization/Yaml/MergeConflictYaml.cpp`, `Serialization/Bundle/ProjectBundle.cpp`, `Serialization/ProjectSerialization.h`, `Engine/Project.cpp`, new `Tests/Serialization/MergeConflictYaml_Test.cpp`
  - approach: write `merge/conflicts.yaml` when conflicts exist and drop the member when none remain. Base, mine and theirs values use the knob and curve encoders. Loading a project with conflicts adds the load warning "N unresolved merge conflicts". Engine keeps the list on `Project` for M97 - Merge Resolution UI.
  - verify: compiled at P3.T5. Conflicts round-trip; the member is absent when the list is empty; loading gives exactly one warning.
  - size: M

- [ ] M96.P3.T5 — Phase 96.3 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green, and the whole corpus matches `expected/`. The build is required.
  - size: S

## Phase 96.4: `natron-merge` CLI and git integration

- [ ] M96.P4.T1 — `natron-merge diff` and `merge`
  - files: new `MergeTool/CMakeLists.txt`, new `MergeTool/main.cpp`, new `MergeTool/DiffPrinter.cpp`, `CMakeLists.txt` (`add_subdirectory(MergeTool)`), the install rules
  - approach: subcommands as in **Design**. The human-readable diff groups changes per node (`~ Grade1 (renamed from Grade3)`, `  white: 1 → 1.2`, `+ Blur2`, `- Crop1`, `  input Source: Read1 → Blur2`). `merge` takes `--scope template:<key>|all`. Exit codes: 0 clean, 1 conflicts, 2 error. Link only `NatronSerialization`.
  - verify: compiled at P4.T3. A ctest runs `natron-merge` on three corpus triples, checks the exit codes, and checks that its `diff` output contains the expected lines.
  - size: M

- [ ] M96.P4.T2 — git merge driver and textconv
  - files: `MergeTool/main.cpp`, new `docs/graph-merge.md`, new `Tests/merge/git_driver_test.sh`, `Tests/CMakeLists.txt`
  - approach: `git-driver %O %A %B` merges into `%A` with scope `all` and exits nonzero on conflict. `textconv` prints `project.yaml`, with node maps in sorted order. The doc gives the `.gitattributes` and `git config` setup and describes the template workflow.
  - verify: compiled at P4.T3. The shell test makes a temp git repo with the driver configured, commits a base, branches, edits both sides with corpus files, and runs `git merge`: a clean scenario merges automatically and a conflicted one exits nonzero with `merge/conflicts.yaml` present.
  - size: M

- [ ] M96.P4.T3 — Final build, full ctest and smoke
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: S

**Verification gate:** the user's matching decision is recorded (P1.T4). Every corpus scenario
merges to its expected graph and conflicts, both through the API and through `natron-merge`.
The git-driver test passes. Full ctest, smoke and CI are green.
`natron-merge` links no Engine or Gui library (`ldd`/`readelf` check).

## Decisions

- 2026-10-11 — **User decision:** a template is "just a bunch of nodes in a script" that lives in a file on disk; merge works on the node set that came from template T.
- 2026-10-11 — **User decision:** the merge is available in the app and on the CLI, and the CLI doubles as a git merge driver.
- 2026-10-11 — **User decision:** the matching policy waits for the P1 spike; the user chooses at P1.T4.
- 2026-10-11 — Conflicts are stored as data in `merge/conflicts.yaml`, never in labels (a nuke_diff lesson), and the merged result defaults to mine at every conflict.
````

### 2026-10-11T00:43:00-04:00 — change-request
- refs: M97 (new milestone)
- New milestone **M97 - Merge Resolution UI**, file `PLAN/MILESTONES/M97-merge-resolution-ui.md`, board row `| M97 | Merge Resolution UI | todo | M96 | [M97-merge-resolution-ui.md](PLAN/MILESTONES/M97-merge-resolution-ui.md) |`. Full file content:

````markdown
# M97 - Merge Resolution UI

The in-app side of M96 - Graph Diff And Merge: importing templates, **Update from Template**,
a visual diff overlay on the node graph, and a merge view that resolves conflicts per knob.
Depends on M96 - Graph Diff And Merge. Freshness-check the briefs (PLAN-FORMAT §5a) against
M96's P1.T4 decision before starting.

## Design (applies to every task)

- **Import Template…** (File menu) calls `Project::importTemplate` from M96 - Graph Diff And
  Merge. **Update from Template…** is in the File menu and in the node-graph context menu on any
  template-tagged node. It finds the template's stored path, asking for the file if the path
  is gone, and merges with base = the stored snapshot, mine = the live scope, and theirs = the
  current file. The result is applied to the live graph as **one undo step** (M95's
  `diffSnapshots` → `GraphOpApply`). The stored base is then updated to theirs, and the merge
  view opens if there are conflicts.
- **Merge view:** a dock panel listing conflicts grouped by node. Selecting one frames the node
  and opens its panel. Each conflict offers Mine, Theirs and Base, plus any options its handler
  offers (for example "keep both keys"), and there are resolve-all-mine and resolve-all-theirs
  buttons. Each resolution is an undoable knob edit. Conflicted nodes carry a **badge glyph** in
  the graph, and their labels are never touched. Opening a file that holds
  `merge/conflicts.yaml` opens the view.
- **Diff overlay:** "Compare with File…" and "Compare with Template Base" tint the graph.
  Added nodes are green, changed ones amber, and removed ones are drawn as translucent ghost
  nodes at their old positions. Selecting a changed node shows a knob diff (base → current) in
  a side panel. The overlay is view-only, and Escape clears it.

## Phase 97.1: Engine and undo glue

- [ ] M97.P1.T1 — Project-level update-from-template and compare
  - files: new `Engine/ProjectMerge.h`, new `Engine/ProjectMerge.cpp`, `Engine/Project.h`, `Engine/CMakeLists.txt`, new `Tests/ProjectMerge_Test.cpp`
  - approach: `ProjectMerge::updateFromTemplate(project, templateKey, newPath)` snapshots the live scope, runs M96's merge, computes operations against the live state, applies them through a caller-supplied applier (so Gui can wrap it in undo), updates the stored base, and returns the conflicts. `ProjectMerge::compare(project, otherSnapshot)` returns a per-node change summary for the overlay.
  - verify: compiled at P1.T3. Headless gtests on two corpus scenarios: a clean update changes only the template nodes and keeps the artist's overrides; a conflicted one returns its conflicts and leaves mine in the graph.
  - size: M

- [ ] M97.P1.T2 — Undoable apply of an operation batch
  - files: `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`
  - approach: `ApplyGraphOpsCommand` holds forward operations and inverse operations (`diffSnapshots(after, before)` over the touched scope). redo and undo apply them through `GraphOpApply` and refresh the NodeGuis (create and remove).
  - verify: compiled at P1.T3. Exercised by the P4.T1 GUI test: undo after Update from Template restores the pre-merge graph exactly.
  - size: M

- [ ] M97.P1.T3 — Phase 97.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 97.2: Merge view and actions

- [ ] M97.P2.T1 — Merge conflict panel
  - files: new `Gui/MergeConflictPanel.h`, new `Gui/MergeConflictPanel.cpp`, `Gui/CMakeLists.txt`, the Gui pane registration (`Gui/Gui20.cpp` or wherever panes are registered)
  - approach: a dock widget backed by `Project`'s conflict list. Conflicts are grouped by node, with a count in the title. Selecting a row frames the node and opens its settings panel. It refreshes when the list changes.
  - verify: compiled at P2.T5. The P4.T1 GUI test opens a conflicted file and sees the panel with the right count.
  - size: M

- [ ] M97.P2.T2 — Per-conflict resolution actions
  - files: `Gui/MergeConflictPanel.cpp`, new `Gui/MergeConflictResolve.cpp`, `Engine/ProjectMerge.cpp`
  - approach: Mine, Theirs and Base buttons, and handler options from the conflict's `handler options`. A resolution sets the knob, curve, input or structure through undoable commands and removes the conflict. Resolve-all applies to every conflict in the list or in the selected node. Undoing a resolution restores the conflict.
  - verify: compiled at P2.T5. A GUI test resolves one conflict each way, then undoes, then resolves all.
  - size: M

- [ ] M97.P2.T3 — Conflict badge on nodes
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`
  - approach: a small warning glyph at the node's top-right corner while the node has unresolved conflicts, in the same style as the existing node-graph glyphs. It is not a label edit, and it updates when the conflict list changes.
  - verify: compiled at P2.T5. A GUI test sees the badge appear on a conflicted node and go away when it is resolved.
  - size: S

- [ ] M97.P2.T4 — Import, update and compare menu actions
  - files: `Gui/Gui20.cpp` (menus), `Gui/NodeGraph35.cpp` or the file that builds the node-graph context menu, `Gui/GuiAppInstance.cpp`
  - approach: File > Import Template…, File > Update from Template… (a list of the project's templates), File > Compare with File…, and a node-graph context menu with "Update from Template" and "Compare with Template Base" on template-tagged nodes. Update applies through `ApplyGraphOpsCommand` and opens the panel when there are conflicts.
  - verify: compiled at P2.T5, and exercised by the P4.T1 GUI test.
  - size: M

- [ ] M97.P2.T5 — Phase 97.2 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 97.3: Diff overlay

- [ ] M97.P3.T1 — Node-graph diff overlay
  - files: new `Gui/NodeGraphDiffOverlay.h`, new `Gui/NodeGraphDiffOverlay.cpp`, `Gui/NodeGraph.h`, `Gui/NodeGraph.cpp`
  - approach: given `ProjectMerge::compare`'s summary, tint added, changed and removed nodes. Draw ghost items for removed nodes, with their wires to surviving neighbours drawn dashed. The overlay is view-only: ghosts are not selectable and not serialized. Escape or "Clear Comparison" removes it.
  - verify: compiled at P3.T3. A GUI test compares against a corpus base and checks the tint counts and ghost count.
  - size: M

- [ ] M97.P3.T2 — Knob diff side panel
  - files: new `Gui/NodeDiffPanel.h`, new `Gui/NodeDiffPanel.cpp`
  - approach: with the overlay active, selecting a changed node lists each changed knob as `name: base → current`. Curves show key-count and key-time changes, and inputs show their source change.
  - verify: compiled at P3.T3. A GUI test selects a changed node and reads the expected rows.
  - size: M

- [ ] M97.P3.T3 — Phase 97.3 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 97.4: Close-out

- [ ] M97.P4.T1 — GUI test of the template workflow
  - files: new `Tests/gui/m97_template_merge.py`
  - approach: on the `Tests/gui/guitest.py` harness:
    1. import a template;
    2. edit it as the artist;
    3. replace the template file with v2;
    4. run Update from Template on a clean scenario and check the graph;
    5. undo, and check the pre-merge graph;
    6. run a conflicted scenario and resolve through the panel;
    7. save, reopen, and check that the conflicts persisted when left unresolved.
  - verify: passes under `Tests/gui/run-gui-test.sh`. The build is required.
  - size: M

- [ ] M97.P4.T2 — AppImage UAT
  - files: `build/appimages/M97-<sha>.AppImage`, `build/appimages/M97-uat.md`
  - approach: build with `tools/ci/local/package.sh` and launch-check it. The checklist: the supervisor-and-artist workflow end to end on a realistic template (Read → Grade → ColorCorrect → Roto-masked Merge → Write), the compare overlay, resolving in the panel, and `natron-merge` on the same files from a shell.
  - verify: the user signs off. **Stop for the user.**
  - size: M

**Verification gate:** full ctest, smoke, `m97_template_merge.py` and CI green. Update from
Template is a single undo step. Conflicts survive save and reopen. The user signs off the UAT.
````

### 2026-10-11T00:44:00-04:00 — change-request
- refs: M98 (new milestone)
- New milestone **M98 - Live Session Core**, file `PLAN/MILESTONES/M98-live-session-core.md`, board row `| M98 | Live Session Core | todo | M95 | [M98-live-session-core.md](PLAN/MILESTONES/M98-live-session-core.md) |`. Full file content:

````markdown
# M98 - Live Session Core

Live multi-user editing: one Natron session **hosts** a project, and others **join** and see
every graph, knob and position edit live, like a shared Google Doc. Each peer renders locally
with its own cache. Only the script is shared. Depends on M95 - Graph Edit Operations
(recording, encoding and applying edits). Presence, per-user undo and viewer following are
M99 - Live Session Presence And Undo.

Scouting facts (2026-10-11): `Qt::Network` is already linked into Engine, Gui and Renderer
(`CMakeLists.txt:69-71`, `QT_COMPONENTS`). Qt WebSockets is not linked, and nothing uses
`QTcpSocket` yet. The existing IPC is `ProcessHandler` (`QLocalServer`, `Engine/ProcessHandler.h:83-253`).

## Design (applies to every task)

- **Host-authoritative (user decision).** The host's GUI process runs a `QTcpServer`. Frames
  are a u32 big-endian length followed by one YAML document. Message types:
  - `hello{protocol, name, natron version}` and `welcome{userId, colour, seq, snapshot}`;
  - `ops{clientSeq, ops}` and `broadcast{seq, originUser, clientSeq, ops}`;
  - `reject{clientSeq, reason}` and `resyncNode{nodeId, NodeSerialization|deleted}`;
  - `checksum{seq, hash}`, `saveRequest` and `bye`.

  No new dependency. A protocol-version mismatch refuses the join with a clear message.
- **Join:** the host sends its full in-memory bundle (`.ntpz` bytes, including `gui.yaml`) and
  its current seq. The client loads it as an unsaved, session-bound project, titled
  `Live: <host project> @ <host>`.
- **Edits.** Recorded local operations go to the host, tagged with a client seq. The host
  assigns a global seq, applies the operations if they came from a client, and broadcasts them
  to everyone, the originator included as its ack. The client applies its own edits
  optimistically. When a broadcast arrives from another user and touches a key (node+knob,
  node+input, node existence, node UI) that the client still has pending, the client skips it:
  the host will apply the client's operation afterwards, so last-writer-wins converges. When an
  operation's target no longer exists on the host, the host sends `reject` plus `resyncNode` to
  the originator.
- **Divergence check:** every 5 s the host broadcasts the hash of the canonical `project.yaml`
  at a seq. A client compares at the same seq and requests a full resync on a mismatch, logging it.
- **Saving.** Only the host writes the file, and autosave runs only on the host. Ctrl+S on a
  client sends `saveRequest`. Save As on a client writes a private copy and leaves the session.
- **Disconnect.** A client that loses the host keeps an unsaved local copy, is told, and can
  save it. When the host closes the session, every client gets `bye`.
- Remote operations are applied under `Suppress` and are **never pushed onto local undo
  stacks**.
- **No password in v1 (user decision).** The host dialog picks a bind address and port and
  warns that anyone who can reach the port can join.

## Phase 98.1: Protocol, host and client (headless)

- [ ] M98.P1.T1 — Session protocol messages and framing
  - files: new `Serialization/Session/SessionProtocol.h`, new `Serialization/Session/SessionProtocol.cpp`, `Serialization/CMakeLists.txt`, new `Tests/Serialization/SessionProtocol_Test.cpp`
  - approach: message structs and their YAML encoding (operations through M95's `GraphOpYaml`). The framing codec is incremental: it takes bytes and yields complete messages, and rejects frames over a 512 MiB cap.
  - verify: compiled at P1.T5. Every message type round-trips; split and coalesced byte streams decode identically; an oversize or garbage frame gives an error and no crash.
  - size: M

- [ ] M98.P1.T2 — Session host
  - files: new `Engine/LiveSession/SessionHost.h`, new `Engine/LiveSession/SessionHost.cpp`, `Engine/CMakeLists.txt`
  - approach: a `QTcpServer` that handles the join handshake (snapshot from the live project through the bundle writer, in memory), sequencing, apply-then-broadcast (applying through `GraphOpApply` under `Suppress`), rejects with `resyncNode`, checksum broadcast, and disconnect cleanup. It also subscribes to the host's own recorder, so the host user's edits are sequenced the same way.
  - verify: compiled at P1.T5, and exercised by P1.T4.
  - size: L

- [ ] M98.P1.T3 — Session client
  - files: new `Engine/LiveSession/SessionClient.h`, new `Engine/LiveSession/SessionClient.cpp`
  - approach: connect and hello; load the welcome snapshot; send local recorded operations with a client seq while keeping a pending-key set; apply broadcasts using the skip rule in **Design**; clear pending entries on ack; handle reject and `resyncNode`; compare checksums and request a full resync on a mismatch.
  - verify: compiled at P1.T5, and exercised by P1.T4.
  - size: L

- [ ] M98.P1.T4 — Headless convergence test
  - files: new `Tests/LiveSessionConvergence_Test.cpp`
  - approach: on loopback, start one host and three clients, each with its own `AppInstance` in the test process. If one process cannot hold several apps, run the clients as `NatronRenderer` subprocesses driven by a test script instead. Each peer makes 200 random edits concurrently from a fixed seed: knob sets, creates, deletes, rewires, renames, and same-knob races between peers. Then quiesce and require every peer's canonical `project.yaml` to be byte-identical to the host's. A dedicated case covers delete-versus-edit, where the reject and resync converge.
  - verify: compiled at P1.T5, where it passes 20 seeds in a row.
  - size: L

- [ ] M98.P1.T5 — Phase 98.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 98.2: App integration

- [ ] M98.P2.T1 — Session-bound project lifecycle
  - files: `Engine/Project.h`, `Engine/Project.cpp`, `Engine/AppInstance.h`, `Engine/AppInstance.cpp`
  - approach: load a project from in-memory bundle bytes. Add a session-bound flag that changes the title and makes save route to `saveRequest` on clients and skip autosave. On disconnect, leave a plain unsaved project with the message from **Design**. Save As on a client leaves the session.
  - verify: compiled at P2.T5. gtests: loading from bytes equals loading from a file; a client save sends `saveRequest` and the host writes the file; after a disconnect, the client project is unsaved and editable.
  - size: M

- [ ] M98.P2.T2 — Host and Join dialogs and the status indicator
  - files: new `Gui/LiveSessionDialogs.h`, new `Gui/LiveSessionDialogs.cpp`, `Gui/Gui20.cpp` (File menu: Host Live Session…, Join Live Session…, Leave Session), `Gui/CMakeLists.txt`
  - approach: the host dialog takes a bind address, port and display name, and shows the security warning. The join dialog takes host, port and display name, with recent hosts remembered in settings. A status-bar indicator shows the session state and the number of peers connected.
  - verify: compiled at P2.T5, and exercised by P2.T3.
  - size: M

- [ ] M98.P2.T3 — Two-process GUI test of interactive edits
  - files: new `Tests/gui/m98_live_session.py`, `Tests/gui/run-gui-test.sh` (if it needs a two-process mode)
  - approach: start a host and a client Natron on loopback. On the client, drag a knob slider, set a keyframe, move nodes, create and connect a node, draw a roto stroke, and add a user knob. On the host, rename and delete a node. After each step, both sides' canonical `project.yaml` must match. Any GUI edit path that misses the recorder is a bug to fix in M95's hooks, filed as a new task in this phase.
  - verify: passes under the GUI harness. The build is required.
  - size: M

- [ ] M98.P2.T4 — Phase 98.2 build, full ctest and smoke
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: S

## Phase 98.3: Close-out

- [ ] M98.P3.T1 — AppImage UAT across two machines
  - files: `build/appimages/M98-<sha>.AppImage`, `build/appimages/M98-uat.md`
  - approach: build with `tools/ci/local/package.sh` and launch-check it. The checklist:
    - host a bench comp on one machine and join from a second (two processes on one host if no second machine);
    - both edit the same comp at once: knobs, nodes, roto, rewiring;
    - the same knob dragged on both sides converges;
    - each side renders on its own;
    - a client save writes the host's file;
    - pulling the network cable on the client leaves a savable local copy.
  - verify: the user signs off. **Stop for the user.**
  - size: M

**Verification gate:** `LiveSessionConvergence_Test` passes 20 seeds; `m98_live_session.py`,
full ctest, smoke and CI are green; the user signs off the UAT.

## Decisions

- 2026-10-11 — **User decision:** host-authoritative with last-writer-wins, peers connected, no offline editing (CRDTs and node locks rejected).
- 2026-10-11 — **User decision:** no session password in v1; the host dialog warns instead.
- 2026-10-11 — Transport: plain TCP with length-prefixed YAML frames over the already-linked `Qt::Network`. This avoids adding Qt WebSockets, and reuses M95's operation encoding.
- 2026-10-11 — Only the host writes the shared file; clients' Save sends a request to the host.
````

### 2026-10-11T00:45:00-04:00 — change-request
- refs: M99 (new milestone)
- New milestone **M99 - Live Session Presence And Undo**, file `PLAN/MILESTONES/M99-live-session-presence-and-undo.md`, board row `| M99 | Live Session Presence And Undo | todo | M98 | [M99-live-session-presence-and-undo.md](PLAN/MILESTONES/M99-live-session-presence-and-undo.md) |`. Full file content:

````markdown
# M99 - Live Session Presence And Undo

Finishes the user's v1 scope for live sessions on top of M98 - Live Session Core: **presence**
(who is connected, and each user's selection and open panels in their colour), **per-user undo**
(Ctrl+Z undoes only your own edits), and **follow a user's viewer**. Depends on M98 - Live
Session Core.

## Design (applies to every task)

- **Presence messages** are separate from operations and are never recorded or saved.
  `presence{userId, selection: [nodeId], openPanels: [nodeId], viewer?: {...}}` is throttled to
  10 Hz and relayed by the host. Each user has a stable colour assigned at welcome.
- **Per-user undo.** Each peer's local undo stack holds only its own commands, because remote
  operations are never pushed to it (M98 - Live Session Core). Undoing a local command changes
  the graph, and the recorder sends the change as new operations, so other users see the undo
  as an ordinary edit. When a remote operation deletes a node that local commands refer to,
  those commands are made obsolete (`QUndoCommand::setObsolete`) so undo skips them. A local
  undo of a knob edit restores the user's own old value even if someone else changed the knob
  since. That is accepted under last-writer-wins, and the status bar notes it.
- **Follow viewer:** a follower mirrors the leader's viewer frame, active viewer input node,
  zoom and pan, and channels. Following stops on any local viewer interaction. The leader's
  state is sent in presence.

## Phase 99.1: Presence

- [ ] M99.P1.T1 — Presence messages and relay
  - files: `Serialization/Session/SessionProtocol.h/.cpp`, `Engine/LiveSession/SessionHost.cpp`, `Engine/LiveSession/SessionClient.cpp`, `Tests/Serialization/SessionProtocol_Test.cpp`
  - approach: add the `presence` message, the 10 Hz client throttle, host relay, and a per-user presence table on each client that emits a Qt signal when it changes. A user's entry is cleared on disconnect.
  - verify: compiled at P1.T4. Protocol round-trip; in a loopback test, client A's selection reaches client B within 200 ms and is cleared when A disconnects.
  - size: M

- [ ] M99.P1.T2 — Remote selection and open panels in the node graph
  - files: `Gui/NodeGui.h`, `Gui/NodeGui.cpp`, `Gui/NodeGraph.cpp`, `Gui/DockablePanel.cpp`
  - approach: a node selected by a remote user gets a coloured outline in that user's colour, plus a small name tag; several users stack their outlines. A node whose panel a remote user has open shows a coloured dot in its settings-panel header. Local selection and open panels feed the presence sender.
  - verify: compiled at P1.T4. The P3.T3 GUI test checks the outline and dot on B after A selects a node and opens its panel.
  - size: M

- [ ] M99.P1.T3 — Connected-users panel
  - files: new `Gui/LiveSessionUsersPanel.h`, new `Gui/LiveSessionUsersPanel.cpp`, `Gui/CMakeLists.txt`, the Gui pane registration
  - approach: a dock listing users with their colour swatch and name, a "you" marker, the host marker, and a Follow toggle used by P3. Double-clicking a user frames their selection.
  - verify: compiled at P1.T4, and exercised by the P3.T3 GUI test.
  - size: M

- [ ] M99.P1.T4 — Phase 99.1 build and test checkpoint
  - files: none
  - approach: `NATRON_BUILD_JOBS=4 tools/ci/local/build.sh debug`, then `tools/ci/local/test.sh ctest debug`.
  - verify: green. The build is required.
  - size: S

## Phase 99.2: Per-user undo

- [ ] M99.P2.T1 — Make local undo safe under remote edits
  - files: `Gui/NodeGraphUndoRedo.h`, `Gui/NodeGraphUndoRedo.cpp`, `Gui/KnobUndoCommand.h`, `Gui/KnobUndoCommand.cpp`, `Engine/LiveSession/SessionClient.cpp`
  - approach: commands record the node IDs they touch. When a remote `DeleteNode` arrives, every local command touching that ID is marked obsolete, and the stack drops it. Commands keep their nodes by ID, not by `NodeGuiPtr`, wherever the stale pointer would otherwise crash. Remote applies never call `pushUndoCommand`, which is asserted in debug builds. An undo whose target changed remotely restores the local old value and posts the status-bar note.
  - verify: compiled at P2.T2, and exercised by P2.T2.
  - size: L

- [ ] M99.P2.T2 — Per-user undo GUI test
  - files: new `Tests/gui/m99_live_undo.py`
  - approach: two processes, A and B:
    - A edits knob k1 and B edits k2; A undoes, and only k1 reverts on both sides;
    - A creates a node and B moves it; A undoes the create, and the node is gone on both sides;
    - B deletes a node A edited; A undoes, and the obsolete command is skipped without a crash;
    - redo is checked for each case.
  - verify: passes under the GUI harness. The build is required.
  - size: M

## Phase 99.3: Follow a user's viewer

- [ ] M99.P3.T1 — Viewer state in presence
  - files: `Serialization/Session/SessionProtocol.h/.cpp`, `Gui/ViewerTab.cpp`, `Gui/ViewerGL.cpp`
  - approach: viewer state is the current frame, the active viewer node ID with its input index, zoom and pan, and the displayed channels. It is captured on change and sent in presence, throttled. A follower applies it without recording it and without moving its own timeline cursor when it is not following.
  - verify: compiled at P3.T3, and exercised by P3.T3.
  - size: M

- [ ] M99.P3.T2 — Follow toggle and auto-stop
  - files: `Gui/LiveSessionUsersPanel.cpp`, `Gui/ViewerTab.cpp`
  - approach: Follow on a user mirrors their viewer state, and only one leader can be followed at a time. Local scrubbing, zoom, pan or a viewer-input change stops following, with a status-bar note. The leader's name is shown in the viewer corner while following.
  - verify: compiled at P3.T3, and exercised by P3.T3.
  - size: M

- [ ] M99.P3.T3 — Presence and follow GUI test, full ctest and smoke
  - files: new `Tests/gui/m99_presence_follow.py`
  - approach: two processes. A selects a node and opens its panel, and B sees the outline and dot. B follows A, A scrubs to frame 20 and zooms, and B matches. B scrubs, and following stops. Then build and run `tools/ci/local/test.sh ctest debug` and `smoke debug`.
  - verify: everything green. The build is required.
  - size: M

## Phase 99.4: Close-out

- [ ] M99.P4.T1 — AppImage UAT
  - files: `build/appimages/M99-<sha>.AppImage`, `build/appimages/M99-uat.md`
  - approach: build with `tools/ci/local/package.sh` and launch-check it. The checklist covers two or three users on one comp: presence colours, the users panel, per-user undo across the cases in P2.T2, and following a viewer during playback.
  - verify: the user signs off. **Stop for the user.**
  - size: M

**Verification gate:** `m99_live_undo.py` and `m99_presence_follow.py` pass, full ctest, smoke
and CI are green, and the user signs off the UAT.

## Decisions

- 2026-10-11 — **User decision:** v1 includes presence, per-user undo, and follow-a-user's-viewer.
- 2026-10-11 — Per-user undo replays the user's own inverse as new operations (last-writer-wins). Commands invalidated by remote deletes are dropped, not transformed.
````

### 2026-10-11T22:51:00-04:00 — change-request
- refs: M100 (new milestone)
- New milestone **M100 - Content Node Hashing**, user-requested on 2026-10-11. Suggested board row, after M93 - Serialization Boundary: `| M100 | Content Node Hashing | todo | M93 | [M100-content-node-hashing.md](PLAN/MILESTONES/M100-content-node-hashing.md) |`.
- **Why:** today `Node::computeHashInternal` (`Engine/Node.cpp:762`) does not hash content. It hashes a `knobsAge` counter, the input hashes, the script name and the project creation time. The user rejected this ("not a reasonable way to do it"). A node's hash must be a content (Merkle) hash: the plugin type, render-affecting knob values, input hashes by index, and the identity of every file read upstream. Any change to a file, the topology or a knob must then change the hash, and identical state must give an identical hash.
- **Suggested project-wide decision** `DECISIONS/2026-10-11-content-node-hashing.md` (+ INDEX line): "Node hashes become content hashes (plugin identity, render-hashed knob values incl. full curves/expressions, inputs by index, file stamps, project digest), replacing knobsAge. Cache entries are evicted LRU, not purged on hash change. User decision 2026-10-11."
- **Collision to fix separately:** the pending live-session entries above claim M95–M99. M95 and M96 already exist on the board (M95 - Knob Cleanup And Keyer Redesign, M96 - Weighted Curve Tangents). The user has not yet chosen new IDs for them. Do not apply them under the colliding IDs; raise it as an open question.
- **Design input:** an Opus investigation on 2026-10-11 read the code but did not build or reproduce anything. Its findings are in the file's Notes. Full file content:

````markdown
# M100 - Content Node Hashing

Replace the `knobsAge` counter hash with a content hash. Identical state gives an identical
hash, so undo and A/B toggling reuse the cache. Any change to a knob, the topology or an
upstream file changes the hash. The change lands in steps: shadow hash plus audit, then file
identity, then switching the cache keys, then deleting the age machinery. Depends on
M93 - Serialization Boundary, because Phases 100.4 and 100.5 touch `Node.cpp` load code and
`NodeSerialization`. M16 - Project Format Redesign deletes the serialized `KnobsAge` field,
as it already plans to.

## Notes (investigation 2026-10-11, code reading only; re-check line numbers)

**Current scheme:**
- `computeHashInternal` hashes the following (`Node.cpp:762-865`), via vector + boost CRC-64 (`Hash64.cpp:42`):
  - the node's age;
  - the IO container's age;
  - inputs as `hash + i`;
  - the script name;
  - the project creation time.
- When the hash changes, the old entries are purged (`:860`), except while a project is loading or closing (`:929`).
- `knobsAge` and `CreationDate` are saved in the project and restored on load (`Node.cpp:1679`, `ProjectPrivate.cpp:125`).
- The viewer and disk cache tables of contents persist across sessions (`Project.cpp:638`, `AppManager.cpp:501`).
- `ImageKey::operator==` ignores the holder (`ImageKey.cpp:82-98`). Cache lookup is by hash plus equality (`Cache.h:1702`), so entries are effectively shared already.

**Bugs in the current scheme.** Found by reading the code; none is reproduced yet, and the Phase 100.6 regressions must cover them:
1. **Stale images after reopen or autosave recovery.** An unsaved edit takes the age from 10 to 11, and its renders persist in the viewer and disk caches. On reopen the age is back at 10. A *different* edit reaches 11 again, which gives the same hash, so the stale pixels are served.
2. **Two copies of one project collide.** Save As keeps the creation date. Once the two copies diverge, equal ages give equal hashes for different content, in two windows or in later sessions.
3. **A file changed on disk stays stale.** No mtime reaches the hash; only Reload fixes it.
4. **Off-thread edits don't move the hash.** Knob edits made off the main thread, or during inputChanged, never bump the age (the `isMT` guard, `EffectInstance.cpp:3430`).
5. **Undo and A/B toggling discard valid cache.** Every return to an earlier state gets a new age.

Minor: inputs are combined with `+i`; `U32 newAge` truncates (`Node.cpp:1941`); paste and clone deliberately force different hashes (`Gui/NodeGraphPrivate10.cpp:249`).

## Design (applies to every task)

- **Node hash** = XXH3 over four parts. Keep `U64`, and never let the result be 0, because 0 means "not computed".
  1. **Identity:** plugin ID, plugin major and minor version, and a binary stamp (the `.ofx` path, mtime and size; for native nodes, the Natron version and git hash).
  2. **Render-hashed knobs**, sorted by script name, each as name plus content.
     - Content per view and dimension: the static value; the *whole* curve (keys, interpolation, tangents, extrapolation; weights after M96 - Weighted Curve Tangents); the expression string; parametric, table and string data.
     - Doubles are canonicalized: −0 becomes +0, and all NaNs become one value.
     - Each knob memoizes its hash behind a dirty bit.
     - Linked or slaved knobs hash the value they read through the link, which replaces the `knobsAgeChanged` slaving (`Node.cpp:5378`).
  3. **Inputs:** each index mixed with the input's hash, or a fixed "unconnected" marker. The viewer hashes its active inputs with their index. `getInput()` already applies group redirection.
  4. **Project digest:** formats, fps, frame range, OCIO config identity and working space, layer-registry content, and the render-affecting preferences.
- **Which knobs are render-hashed:** value knobs that are not hash-exempt and are persistent or evaluate on change. Erring towards including a knob is the safe direction, since the cost is only a cache miss.
  - Explicitly exempt: label, sublabel, preview, hide-inputs, info text and user colour.
  - Do NOT exempt a knob just because it doesn't evaluate on change: secret persistent OFX state params are often flagged that way.
- **What leaves the hash:** the script name and the creation time. One exception: a node with an expression also folds in its fully qualified name, because `thisNode` and `thisGroup` are resolved by name.
- **Expressions** fold in the content hashes of their dependency knobs (`getExpressionDependencies`, with a visited set). Tokens such as `random`, `time.`, `open(` and `os.` mark the knob volatile.
- **Volatile:** a per-session salt, reused within a session but never served from the disk cache in a later one. It applies to volatile expressions and to plugins on a "volatile" list (Text, which depends on fontconfig, etc.).
- **Roto and RotoPaint:** `RotoItem::appendContentHash`, memoized per item and dirtied wherever `incrementNodesAge()` is called today. It covers the item's knobs, control and feather points with their curves, stroke points, and the order of items. Each stroke's internal nodes fold in their own item's hash.
- **Read/Write containers:** the container's knobs are hashed into the embedded node. This replaces M58's age fold.
- **Files:**
  - A `FileStampRegistry` holds (path, statx mtime_ns, size, inode) with a short TTL.
  - Single files: `KnobFile`, the OCIO config, LUTs, fonts and DeepRead layers. A Write's output file is not stamped.
  - Sequences: a sequence stamp, the hash of every frame's stamp, taken from the existing directory scan. Rescan on inotify (`QFileSystemWatcher`), on app focus-in (NFS gets no inotify), and on Reload.
  - Per-render guard: stat each Read's current frame when render args are set up, and restart the render if the stamp moved. Keep this guard off the serial render path (M90 - Render Path Serial Overhead, M82 profiler).
- **Animation:** hash the whole curve. Time stays in `ImageKey` for frame-varying nodes, as today. Hashing the value at each frame is a later optimization that can ride the per-request hash of M71 - RFV Render Context.
- **Eviction:** no purge on hash change; LRU reclaims old entries. Explicit purges stay on node delete, Reload, Clear Cache and a cache-version change. Raise `ActionsCache`'s instance limit so toggling between two states keeps both.
- **Threading:** off-thread setters set an atomic dirty flag and post `mustComputeHashOnMainThread`. `getHashValue()` and the render-args snapshot recompute a dirty hash under the node lock. The main-thread assert goes away.
- **Persistence:** stop reading `KnobsAge` on load and write 0 until M16 - Project Format Redesign removes it. M93's golden harness already strips it. Bump `NATRON_CACHE_VERSION` when the keys switch over.
- **GPU vs CPU:** not part of the key. Their outputs differ by about a rounding step and already mix in the cache today.
- **Related milestones:**
  - M92 - Linked Nodes gets simpler if this lands first, since link groups hash their shared storage.
  - M19 - USD/Hydra Foundation's `stackHash` should reuse the knob hash.

## Phase 100.1: Hasher and knob content hash
- [ ] M100.P1.T1 — Vendor XXH3 and add a streaming ContentHasher
  - files: libs/xxhash/xxhash.h, libs/CMakeLists.txt, Engine/ContentHasher.h, Engine/ContentHasher.cpp, Tests/ContentHasher_Test.cpp
  - approach: header-only XXH3-64 behind ContentHasher (appendU64, appendDouble with canonicalization, length-prefixed appendString and appendBytes; finalize never returns 0). First check whether the container's OIIO `hash.h` exposes XXH3; if it does, use it and skip vendoring.
  - verify: `ctest -R ContentHasher` covers known vectors, -0/+0 and NaN canonicalization, and ("ab","c") ≠ ("a","bc")
  - size: M
- [ ] M100.P1.T2 — Add memoized KnobI::appendContentHash for every knob type
  - files: Engine/Knob.h, Engine/Knob.cpp, Engine/KnobImpl.h, Engine/KnobTypes.cpp, Engine/Curve.cpp
  - approach: for each view and dimension, hash the static value, the full curve and the expression string. Memoize behind a dirty bit that is cleared in onInternalValueChanged and on curve or expression changes. Linked dimensions hash through their master.
  - verify: Tests/KnobContentHash_Test.cpp: every kind of mutation changes the hash, reverting restores it, and a slaved knob hashes equal to its master
  - size: L
- [ ] M100.P1.T3 — Classify render-hashed knobs and exempt UI-only ones
  - files: Engine/Knob.h, Engine/Knob.cpp, Engine/Node.cpp, Engine/OfxParamInstance.cpp
  - approach: isRenderHashed() = value type && !hashExempt && (persistent || evaluateOnChange). Call setHashExempt on label, sublabel, preview, hide-inputs, info text and user colour.
  - verify: a test shows that editing the label or info text leaves the hash unchanged, and editing a secret persistent param changes it
  - size: M

## Phase 100.2: Node content hash in shadow
- [ ] M100.P2.T1 — Compute the Node content hash beside the age
  - files: Engine/Node.cpp, Engine/Node.h, Engine/NodePrivate.h, Engine/Project.cpp, Engine/Project.h
  - approach: identity + sorted hashed knobs + inputs by index (mixed, with an unconnected marker) + the project digest, stored as contentHash. getHashValue is unchanged.
  - verify: Tests/NodeContentHash_Test.cpp: two freshly built identical graphs hash equal; topology changes and input swaps produce different hashes
  - size: M
- [ ] M100.P2.T2 — Hash roto/paint items and IO containers
  - files: Engine/RotoItem.cpp, Engine/RotoDrawableItem.cpp, Engine/Bezier.cpp, Engine/RotoStrokeItem.cpp, Engine/Node.cpp
  - approach: a per-item memoized appendContentHash, dirtied wherever incrementNodesAge is called. Stroke internal nodes fold in their item's hash, and the container's knobs fold into the embedded IO node.
  - verify: moving one control point changes the hash and undo restores it; a micro-benchmark measures hash cost on a 5k-stroke comp
  - size: L
- [ ] M100.P2.T3 — Fold expression dependencies into knob hashes
  - files: Engine/Knob.cpp, Engine/Knob.h, Engine/Node.cpp
  - approach: fold in each dependency knob's content hash (visited set to stop cycles), the owner's fully qualified name when the node has expressions, and a session salt when a volatile token appears.
  - verify: a test shows that changing a referenced knob on a node that is not upstream changes the hash, and that an expression cycle terminates
  - size: M
- [ ] M100.P2.T4 — Add a NATRON_HASH_AUDIT shadow cross-check
  - files: Engine/Node.cpp, Engine/Node.h, Tests/CMakeLists.txt
  - approach: log once per knob whenever the age and content hashes disagree, in either direction. Run the full ctest with the audit on and record what it finds in `## Decisions`.
  - verify: an audit ctest run in which every discrepancy is explained or fixed
  - size: M

## Phase 100.3: File identity
- [ ] M100.P3.T1 — Add FileStampRegistry and single-file stamps
  - files: Engine/FileStampRegistry.h, Engine/FileStampRegistry.cpp, Engine/KnobFile.cpp, Engine/Project.cpp, Tests/FileStamp_Test.cpp
  - approach: statx stamps with a TTL memo. The hashes of KnobFile and the OCIO config include the stamp, which is re-validated at render start.
  - verify: touching the file changes the hash; an unchanged file keeps a stable hash
  - size: M
- [ ] M100.P3.T2 — Add sequence stamps with inotify and focus-in rescan
  - files: Engine/FileStampRegistry.cpp, the native Read node source (M75 - Native Read), the DeepRead source, Engine/PrecompNode.cpp, Gui/GuiApplicationManager.cpp
  - approach: hash all frame stamps from the existing directory scan. Watch the directory with QFileSystemWatcher, rescan on app focus-in, and force a rescan on Reload.
  - verify: rewriting one frame of a sequence changes the Read's hash; confirm the viewer re-renders in a GUI test or UAT
  - size: L
- [ ] M100.P3.T3 — Add a per-render current-frame stat guard
  - files: Engine/ParallelRenderArgs.cpp, Engine/FileStampRegistry.cpp, Engine/ViewerInstance.cpp
  - approach: stat each Read's frame file when render args are set up. On a mismatch, update the registry, recompute the hash, abort and restart the render.
  - verify: a test overwrites a frame between two renders and gets the new pixels; the M82 profiler shows no serial regression
  - size: M

## Phase 100.4: Switch keys and drop the purge
- [ ] M100.P4.T1 — Make getHashValue return the content hash and bump the cache version
  - files: Engine/Node.cpp, Global/GlobalDefines.h, Engine/Hash64.cpp, Tests/BaseTest.cpp
  - approach: remove the script name (except on nodes with expressions), the creation time and the age from the key. Set NATRON_CACHE_VERSION to 10.
  - verify: the existing hash tests pass; two identical nodes share one cache entry (a counting effect renders once)
  - size: M
- [ ] M100.P4.T2 — Replace purge-on-hash-change with LRU eviction
  - files: Engine/Node.cpp, Engine/EffectInstancePrivate.cpp, Tests/DeepRenderPipeline_Test.cpp, Tests/DeepImageCache_Test.cpp
  - approach: remove the purge in computeHashInternal and keep the delete, Reload and Clear Cache purges. Raise the ActionsCache instance limit. Rewrite the tests that assert a purge as tests that assert reuse.
  - verify: going value 1 → 2 → 1 causes zero re-renders on the third step
  - size: M
- [ ] M100.P4.T3 — Make the hash recompute lazy and thread-safe
  - files: Engine/Node.cpp, Engine/Node.h, Engine/EffectInstance.cpp, Engine/Knob.cpp
  - approach: an atomic dirty flag that any thread can set. getHashValue and the render snapshot recompute under the node lock. Remove the main-thread assert; edits during inputChanged mark the node dirty.
  - verify: a test sets a knob from a worker thread and the next render sees the new hash; run TSan if it is available
  - size: L

## Phase 100.5: Remove knobsAge
- [ ] M100.P5.T1 — Turn core age bumps into dirty marks and delete the age hacks
  - files: Engine/Node.cpp, Engine/NodeInputs.cpp, Engine/EffectInstance.cpp, Engine/Project.cpp, Engine/Node.h
  - approach: incrementKnobsAge becomes markHashDirty. Delete the knobsAgeChanged slaving, the group-input force, the group-disable loop, the format loop and the layer-user bump.
  - verify: full ctest, plus disabled-Group and GroupInput rewire tests
  - size: L
- [ ] M100.P5.T2 — Migrate the remaining age-bump callers
  - files: Engine/RotoDrawableItem.cpp, Engine/RotoStrokeItem.cpp, AddLayers and DeepAddLayers sources, Gui/NodeGraphPrivate10.cpp, Gui/MultiInstancePanel.cpp
  - approach: apply the same replacement, and delete the bump that forces paste and clone to hash differently.
  - verify: `grep incrementKnobsAge` finds nothing; ctest passes
  - size: M
- [ ] M100.P5.T3 — Stop reading KnobsAge on load
  - files: Engine/Node.cpp, Engine/NodeSerialization.cpp (or M93's plain struct)
  - approach: ignore the field on load and write 0; M16 - Project Format Redesign deletes it.
  - verify: the M93 golden harness is unchanged; load → save → load gives an identical hash
  - size: S

## Phase 100.6: Invariant tests and cost
- [ ] M100.P6.T1 — Add a hash-invariant and regression suite
  - files: Tests/HashInvariants_Test.cpp, Tests/CMakeLists.txt
  - approach: the same state gives the same hash (fresh build, save/load, a second app instance); every edit changes it; undo restores it; topology changes and file touches change it; label and position changes don't. Add regressions for Notes bugs 1, 2 and 4.
  - verify: `ctest -R HashInvariants`
  - size: M
- [ ] M100.P6.T2 — Add a plugin sweep: every render-hashed knob of every bundled plugin changes the hash
  - files: Tests/HashPluginSweep_Test.cpp, Tests/CMakeLists.txt, Tests/hash_exempt_allowlist.txt
  - approach: create each plugin from appPTR's list. For each isRenderHashed knob, perturb it by type, assert the hash changed, revert, and assert it is restored. The allowlist explains every exemption.
  - verify: ctest passes with the OFX bundle
  - size: L
- [ ] M100.P6.T3 — Benchmark hash cost
  - files: tools/bench/hash_bench.py
  - approach: time the hash recompute on the bench comps and on a heavy roto comp. Target: keystroke to rehash under 1 ms on HD comps.
  - verify: record the results in `## Decisions`
  - size: M

**Verification gate:** full ctest green in both scheduler modes; the HashInvariants and HashPluginSweep suites pass; the audit run leaves no discrepancy unexplained; the regressions for bugs 1, 2 and 4 pass; value toggling and undo hit the cache; touching a frame file refreshes the viewer in the AppImage; `incrementKnobsAge`/`knobsAge` appear nowhere outside serialization.
````

### 2026-10-12T00:33:00-04:00 — change-request
- refs: M101 (new milestone), M84, M85
- New milestone **M101 - Clean-Sheet Merge Node**, user-requested on 2026-10-12. It is a new native `fr.natron.Merge` that matches Nuke's Merge feature for feature, is layer-aware with universal, per-layer or per-channel alpha, and has a native multi-input GPU kernel. It replaces today's `MergeNode`, which is the openfx-misc Merge 2.0 port, with no compatibility. M101 was chosen because the pending entries above already claim M95–M100.
- Suggested board row, **directly after M84 - GPU Placement And Residency** and before M81 - Raw GPU Kernels and M85 - Native GPU Kernels: `| M101 | Clean-Sheet Merge Node | todo | M84 | [M101-clean-sheet-merge-node.md](PLAN/MILESTONES/M101-clean-sheet-merge-node.md) |`
- Run the §5a freshness check at promotion. Its GPU tasks were written against M84's branch at `8bf4a189a`, before the gate.
- **Suggested project-wide decision** `DECISIONS/2026-10-12-clean-sheet-merge.md` (+ INDEX line): "Merge is rewritten as native `fr.natron.Merge`, with Nuke's formulas made HDR-safe, layer modes (Single/All matching/List) and per-side alpha sources (Universal/Per-layer/Per-channel alpha layer). Dropped: alpha masking, mask fringe/inject, video colorspace. The old Merge IDs are deleted with no compatibility, and old projects get the missing-plugin node. User decision 2026-10-12."
- Full file content:

````markdown
# M101 - Clean-Sheet Merge Node

A new native Merge, `fr.natron.Merge`. It matches Nuke's Merge feature for feature, minus a few
pieces of legacy cruft, and it can merge several layers at once. Each side's alpha can be one
universal channel, each layer's own alpha, or a per-channel alpha layer. It ships with a CPU
row kernel and a native multi-input GPU kernel, using the placement and residency interface from
M84 - GPU Placement And Residency, so merges join resident regions instead of breaking them.
The current `MergeNode` is deleted with **no compatibility**. It was a faithful port of
openfx-misc Merge 2.0, built in M67 - Native Core Nodes. Old projects that use it load the
missing-plugin node (user, 2026-10-12).

Scouted facts (2026-10-12, `main` with M84 at its gate on `origin/milestone/m84-gpu-placement-and-residency` `8bf4a189a`):
- **Today's Merge** is `Engine/Nodes/Merge/Merge.{h,cpp}` (`MergeNode`) plus `MergeOperators.{h,cpp}`.
  - It keeps the OFX IDs `net.sf.openfx.MergePlugin` and the nine `net.sf.openfx.Merge<Preset>` presets, each registered as a separate plug-in.
  - Inputs are B=0, A=1, Mask=2, then A2..A64.
  - Channels are r/g/b/a bools for A, B and Output (`Merge.cpp:679-691`).
  - The `screenAlpha` "Alpha masking" knob is at `:667`.
  - Each plane in `args.outputLayers` is merged with the same plane on both sides (`:973-1018`). There is no separate A, B or output layer selection, and alpha always comes from the plane's own channel 3.
  - It has its own `render()` and mask/mix loop (`:1167-1187`), because `NativeImageEffect::render` fills only `src[0]` (`NativeImageEffect.cpp:466-469, 543-544`) even though `RowIO` holds `kPixelKernelMaxInputs = 4` sources (`PixelKernel.h:37,89`).
  - Several operators branch on or clamp against `maxValue = 1`: colour-dodge/burn, reflect, freeze, overlay and pin-light (`MergeOperators.cpp:150-200`).
- **Users of the old node:**
  - **RotoPaint internals** create Merges by `PLUGINID_OFX_MERGE` and set `operation` by name: `RotoContext.cpp:71,4674`, `RotoDrawableItem.cpp:65,261-268`, `RotoItem.cpp:57`, `RotoLayer.cpp:58`, `RotoStrokeItem.cpp:60`.
  - **Presets:** `AppInstance.cpp:1068`.
  - **Further references:** `AppManager.cpp`, `Node.cpp`, `ParallelRenderArgs.cpp`, `EffectInstance.h`, `NodeDocumentation.cpp` and `Engine/Nodes/README.md` (§148, §585, §615, the tolerance table at §658-676).
  - **PyPlugs:** `DropShadow`, `EdgeBlur`, `Fill`, `Glow`, `LightWrap`, `PIKColor` and `ZMask` under `Gui/Resources/PyPlugs/`.
  - **Tests:** `Tests/Native/NativeMerge_Test.cpp` and `MergeOperators_Test.cpp`. Merge is also used in `ColorViewsRender_Test`, `DefaultChannelSet_Test`, `GLScheduler_Test`, `LayerKnobs_Test`, `NativeMetadataPolicy_Test`, `NativeMetadata_Test`, `NodeCategory_Test`, `OfxMetadataBridge_Test`, `PoolSharing_Test`, `Native/NativePluginList_Test`, `Native/PyPlugNativeParams_Test` and `Native/BlurFIRGaussian_Test`.
- **Layers:**
  - A layer is an `ImageLayerDesc` with 1–4 channels.
  - **No layer has its own alpha.** `beauty.A` is just a channel named A. Only the colour plane has the `rgba`/`rgb`/`alpha`/`xy` views (`DESIGN/2026-09-26-rgba-rgb-alpha-layers.md`).
  - Missing channels read zero.
  - Layer knobs are declared with `Node::declareLayerKnob(knob, inputNb, role)` (`Node.h:1626`). Input-bound knobs list the input's layers, and Target knobs list the registry plus "New layer…". The registry doc names "Merge A/B selects" as input-bound and "Merge output" as a target (`DESIGN/2026-09-19-layer-registry.md` §3).
  - Planes come from `getComponentsNeededAndProduced` (Shuffle's is at `Shuffle.cpp:615-672`).
  - Mask and mix are in the base class (`NativeImageEffect.h:171-182`).
- **GPU interface (M84 - GPU Placement And Residency):**
  - `Engine/Gpu/GpuRender.h` defines `GpuQuery`, `GpuImageView` and `GpuRenderArgs`, the latter with a vector of input views.
  - The virtuals on `NativeImageEffect` are `canRenderOnGpu`, `getGpuInputRoI` and `renderGpu`.
  - Kernels are Slang, compiled offline to SPIR-V and listed in `GpuKernelRegistry`. Storage buffers go in set 0 and push constants are at most 128 B.
  - `Engine/Gpu/Kernels/pointops.slang` has a single-input `IPixelOp { float4 apply(float4 v, uint2 xy); }` plus `Chain`.
  - M84's decisions note that placement cannot chain one region into another while every GPU node has a single input.

## Design

- **Identity:**
  - ID `fr.natron.Merge`, version 1, label "Merge", category Merge.
  - Inputs are **B = 0, A = 1, Mask = 2, then A2..A64**, Nuke's order and the old node's. A inputs fold in order like Nuke: A over B, then A2 over the result, and so on.
  - The presets (Plus, Matte, Multiply, In, Out, Screen, Max, Min, Difference) are **menu entries** that create `fr.natron.Merge` with `operation` set. They are not separate plug-ins.
- **Operators:**
  - All of Nuke's operators: atop, average, colour-burn, colour-dodge, conjoint-over, copy, difference, disjoint-over, divide, exclusion, from, geometric, hard-light, hypot, in, mask, matte, max, min, minus, multiply, out, over, overlay, plus, screen, soft-light, stencil, under and xor.
  - Plus the openfx-misc extras: hue, saturation, colour, luminosity, freeze, grain-extract, grain-merge, pin-light and reflect.
  - **Maths are Nuke's formulas, made HDR-safe** (user). Where a Nuke formula clamps, flips sign, divides by zero or jumps for values above 1 or below 0, the new node uses a continuous unbounded variant. Every such deviation is listed in the maths header and recorded under `## Decisions`. There is no legacy toggle.
- **Operator source, written once:**
  - The operators live in one header, `Engine/Nodes/Merge/MergeMath.h`, as scalar and `float4` functions in a C++/Slang common subset, behind a small compat shim.
  - The CPU row kernel and `merge.slang` both include it, so the two backends cannot drift.
  - If the shared subset becomes contorted, fall back to two sources plus a golden cross-backend test, and record that as a decision.
- **What is merged, the `layers` knob:**
  - **Single:** A layer → B layer → output layer. The output is a Target knob that may create a layer and defaults to the B layer.
  - **All matching:** every layer present on A and on B, paired by ID.
  - **List:** the chosen layers, Nuke's "also merge".
  - In every mode, all other B layers pass through, and a layer missing on a side reads as zero.
- **Alpha source, per side (A and B separately):**
  - **Universal:** one channel, default `rgba.A`, is that side's alpha for every merged layer. `none` means alpha 0.
  - **Per-layer:** each layer's own channel named `A` or `alpha` (case-insensitive) is its alpha. A layer without one falls back to the universal channel.
  - **Per-channel:** an alpha **layer** whose channel *i* is the alpha of merged channel *i*, e.g. `alphaRGB` for `beauty.RGB`. A channel-count mismatch is a render error naming both layers.
  - **Output alpha:**
    - With Universal, the universal alpha channel is merged with the alpha rule when it is in the output.
    - With Per-layer, each layer's alpha channel is merged as part of that layer.
    - With Per-channel, the alpha layer itself is merged per channel and written to B's alpha layer.
- **Other knobs:**
  - set bbox to (union, intersection, A, B)
  - metadata from (A, B)
  - frame range from (A, B)
  - mix
  - mask through the base class's mask channel, invert and mix
- **Dropped on purpose (user):** Nuke's mask fringe and inject, video colorspace, and **alpha masking** (`screenAlpha`). Blend operators process alpha like any other channel, and users mask or premult upstream.
- **Region awareness:**
  - `getRegionOfDefinition` follows the bbox mode.
  - On CPU and GPU, rows or strips outside every A input's RoD copy B, with no operator call and no dispatch.
  - `isIdentity` returns B when no A is connected and the operator is identity for a transparent A.
- **GPU:**
  - `merge.slang` defines `IBinaryPixelOp { float4 apply(float4 a, float aa, float4 b, float ba, uint2 xy); }` and one kernel that merges a layer list with in-kernel mask and mix.
  - `canRenderOnGpu` accepts every operator. Non-separable operators need 3 or more channels and are refused on GPU otherwise.
  - Multiple A inputs fold in one dispatch, up to the push-constant and binding limits, and in chained dispatches past that, keeping the result resident.
  - Hand-off to M87 - Point-Op Kernel Fusion: `IBinaryPixelOp` is the fusion hook for merges.

## Phase 101.1: Operator maths

- [ ] M101.P1.T1 — Write the single-source merge operator header
  - files: `Engine/Nodes/Merge/MergeMath.h` (new), `Engine/Nodes/Merge/MergeMathCompat.h` (new), `Tests/Native/MergeMath_Test.cpp` (new), `Tests/CMakeLists.txt` if the `Native/*_Test.cpp` glob does not pick it up
  - approach:
    - Implement every operator in the Design list as pure functions of premultiplied `(A, a, B, b)` per channel, plus `float4` forms for the four non-separable ones.
    - Use Nuke's documented formulas. Then make each HDR-safe where the formula clamps at 1, flips sign, divides by zero or jumps outside [0, 1]: for example, divide with B = 0 → 0, colour-dodge/burn without the `min(1, …)` clamp, and overlay/soft-light/hard-light branches that stay continuous above 1.
    - List every deviation from Nuke in a table in the header comment.
    - The compat shim maps `float4`, `min`/`max`/`abs`/`sqrt`/`pow` and the function qualifiers so the header compiles as both C++20 and Slang. Prove the Slang side compiles with `slangc` in the test's CMake, or in a build step, if `slangc` is available to the build (check how M84 compiles `Engine/Gpu/Kernels`).
    - Do not include or copy `MergeOperators.*`.
  - verify: `MergeMath_Test` has golden values per operator at in-range, above-1, negative and zero-alpha inputs, checked against Nuke's formula, or against the documented deviation where one applies. The header compiles under `slangc`. Both run at the milestone gate (one build per milestone).
  - size: L

## Phase 101.2: Node on the CPU

- [ ] M101.P2.T1 — Feed multiple inputs to point-op kernels in NativeImageEffect
  - files: `Engine/Nodes/Image/NativeImageEffect.{h,cpp}`, `Engine/Nodes/Image/PixelKernel.h`, `Tests/Native/NativeImageEffect_Test.cpp`
  - approach:
    - Add a trait or virtual that lists the point-op source inputs (default `{0}`). `render()` then fetches each listed input's plane for the row, fills `src[i]`/`nSrc`, and passes a per-source "pixel present" span for rows outside an input's RoD, as `MergeOperators::RowSide::present` does today.
    - Raise `kPixelKernelMaxInputs` if needed. A nodes beyond the limit fold in a loop inside the kernel's caller, so do not raise it to 66.
    - The base class's unpremult, kernel, premult and mask × mix order is unchanged.
    - Existing single-input nodes must be bit-identical.
  - verify: a new `NativeImageEffect_Test` case renders a two-input test kernel with an offset second input. All existing native node tests still pass at the gate.
  - size: M

- [ ] M101.P2.T2 — Add the new Merge node with a single layer and universal alpha
  - files: `Engine/Nodes/Merge/NativeMerge.{h,cpp}` (new), `Engine/AppManager.cpp` (registration), `Tests/Native/NativeMergeNode_Test.cpp` (new)
  - approach:
    - `fr.natron.Merge` v1 with the Design's inputs, operation, bbox, metadata from, frame range from, and the base class's mask and mix.
    - Use `layers = Single` only for now, with A/B/output layer knobs declared through `declareLayerKnob` (A and B input-bound, output a Target) and universal alpha channel selects per side.
    - The kernel calls `MergeMath.h` through P2.T1's multi-input `RowIO`, folding A2..An in order.
    - Override `getRegionOfDefinition`, `isIdentity` and `deriveOutputMetadata` per the Design.
    - Rows outside every A's RoD copy B.
    - Name the class so it does not clash with `MergeNode` (deleted in P5) or the `Merge` namespace in `MergingEnum.h`.
  - verify: `NativeMergeNode_Test` covers over with an offset A, three A inputs, each bbox mode, mask and mix, identity with no A, a non-separable operator, and output-layer creation. It runs at the gate.
  - size: L

- [ ] M101.P2.T3 — Turn the old Merge presets into menu entries for the new node
  - files: `Engine/AppInstance.cpp` (`:1068` preset handling), `Engine/AppManager.cpp`, the node-menu or tab-menu preset source (find where presets are listed for M45 - Native Tab Menu), `Tests/Native/NativeMergeNode_Test.cpp`
  - approach:
    - Plus, Matte, Multiply, In, Out, Screen, Max, Min and Difference create `fr.natron.Merge` with `operation` set, and show the operator as the node's sub-label.
    - They are not separate plug-in IDs. Register the menu entries alongside the old presets until P5 deletes them.
  - verify: a test creates each preset entry and checks the plug-in ID and operation. Manual check: the tab menu lists them.
  - size: M

## Phase 101.3: Layers and alpha modes

- [ ] M101.P3.T1 — Add the `layers` modes: Single, All matching and List
  - files: `Engine/Nodes/Merge/NativeMerge.{h,cpp}`, `Tests/Native/NativeMergeNode_Test.cpp`
  - approach:
    - Add the `layers` choice.
    - All matching pairs the layers present on A and B by ID.
    - List uses a multi-layer knob, input-bound to B.
    - `getComponentsNeededAndProduced` asks each A input and B for the merged planes, produces those planes, and passes everything else through from B.
    - A layer missing on a side reads zero.
    - Follow the widen-on-write rules in `DESIGN/2026-09-26-rgba-rgb-alpha-layers.md` §3 for colour views.
  - verify: tests merge `rgba` + `beauty` + `spec` in All matching, a two-layer List, and a layer absent on A. All other layers on B are untouched.
  - size: M

- [ ] M101.P3.T2 — Add the per-side alpha source modes: Universal, Per-layer and Per-channel
  - files: `Engine/Nodes/Merge/NativeMerge.{h,cpp}`, `Tests/Native/NativeMergeNode_Test.cpp`
  - approach:
    - Add a per-side `alpha source` choice, plus the universal channel select and the per-channel alpha-layer select (input-bound).
    - Per-layer finds the layer's channel named `A`/`alpha`, case-insensitively, and falls back to the universal channel.
    - Per-channel requires the alpha layer's channel count to equal the merged layer's, and otherwise errors with both names.
    - Output alpha follows the Design.
    - The kernel takes an alpha span per channel. Universal and per-layer broadcast one span, and per-channel binds one per channel, so all three modes share one row path.
  - verify: tests check each mode on each side, including A per-layer over B universal, and per-channel with `alphaRGB` against a hand-computed result. The per-layer fallback and the mismatch error are covered.
  - size: L

## Phase 101.4: GPU path

- [ ] M101.P4.T1 — Write the Merge Slang kernel and register it
  - files: `Engine/Gpu/Kernels/merge.slang` (new, includes `MergeMath.h`), `Engine/Gpu/GpuKernelRegistry.cpp`, `Engine/Gpu/CMakeLists.txt`, `Engine/Nodes/Merge/NativeMerge.{h,cpp}`
  - approach:
    - Add `IBinaryPixelOp` and one kernel parameterised by operator: a specialisation constant, or one entry point per operator if that is how the registry works.
    - The kernel takes a layer list, per-channel alpha bindings, mask, mix, and up to N A inputs folded in one dispatch. Past N, chain dispatches and keep the result resident.
    - `canRenderOnGpu` per the Design. `getGpuInputRoI` returns the output rect, as for a point op.
    - Strips outside every A's RoD copy B, with no operator dispatch.
    - Check M84's merged `GpuRender.h` first. Its interface may have shifted at the gate.
  - verify: covered by P4.T2's parity test.
  - size: L

- [ ] M101.P4.T2 — Test CPU/GPU parity and region chaining for Merge
  - files: `Tests/Gpu/GpuMerge_Test.cpp` (new), `Tests/CMakeLists.txt` or `Tests/Gpu/CMakeLists.txt`
  - approach:
    - On lavapipe, compare the GPU and CPU output for every operator × alpha mode, with two A inputs, mask and mix. Use exact tolerance for Porter-Duff operators and transcendental for blend modes, per `Engine/Nodes/README.md`'s tolerance table.
    - Placement test: Grade → Merge(A = Blur(B), B) → Grade is one resident region with one upload per external input. This is the region-to-region case M84 could not build.
  - verify: the test runs in CI's lavapipe job and at the gate.
  - size: M

## Phase 101.5: Replace the old Merge

- [ ] M101.P5.T1 — Move RotoPaint's internal merges to the new node
  - files: `Engine/RotoContext.cpp`, `Engine/RotoDrawableItem.cpp`, `Engine/RotoItem.cpp`, `Engine/RotoLayer.cpp`, `Engine/RotoStrokeItem.cpp`
  - approach:
    - Create `fr.natron.Merge` instead of `PLUGINID_OFX_MERGE`.
    - Map each knob set by name (`operation` and any channel or bbox knobs) to the new knobs. Input indices are unchanged (B = 0, A = 1, Mask = 2).
  - verify: the existing Roto/RotoPaint tests, plus a manual paint-stroke check in the GUI, at the gate.
  - size: M

- [ ] M101.P5.T2 — Move the bundled PyPlugs to the new node (Glow, LightWrap, DropShadow, EdgeBlur)
  - files: `Gui/Resources/PyPlugs/Glow.py`, `LightWrap.py`, `DropShadow.py`, `EdgeBlur.py`
  - approach: replace the old Merge IDs with `fr.natron.Merge` and set `operation` explicitly where a preset ID was used. Input indices are unchanged. Map any `AChannels`/`BChannels`/`OutputChannels`/`screenAlpha` settings to the new layer and alpha knobs. `screenAlpha` has no equivalent, so note any PyPlug whose output changes.
  - verify: `PyPlugNativeParams_Test` (updated in P5.T4) and rendering each PyPlug on a fixture at the gate.
  - size: M

- [ ] M101.P5.T3 — Move the bundled PyPlugs to the new node (Fill, PIKColor, ZMask)
  - files: `Gui/Resources/PyPlugs/Fill.py`, `PIKColor.py`, `ZMask.py`
  - approach: as P5.T2.
  - verify: as P5.T2.
  - size: M

- [ ] M101.P5.T4 — Point the tests that use Merge incidentally at the new node
  - files: `Tests/ColorViewsRender_Test.cpp`, `Tests/DefaultChannelSet_Test.cpp`, `Tests/GLScheduler_Test.cpp`, `Tests/LayerKnobs_Test.cpp`, `Tests/PoolSharing_Test.cpp`
  - approach: swap the ID and the knob names. Keep each test's intent. Where a test asserted old-node behaviour that no longer exists (alpha masking, OFX defaults), rewrite the assertion for the new behaviour and say so in the report.
  - verify: the five tests pass at the gate.
  - size: M

- [ ] M101.P5.T5 — Point the metadata, catalogue and PyPlug tests at the new node
  - files: `Tests/NativeMetadataPolicy_Test.cpp`, `Tests/NativeMetadata_Test.cpp`, `Tests/NodeCategory_Test.cpp`, `Tests/OfxMetadataBridge_Test.cpp`, `Tests/Native/NativePluginList_Test.cpp` (and `Tests/Native/PyPlugNativeParams_Test.cpp`, `Tests/Native/BlurFIRGaussian_Test.cpp` if they reference Merge)
  - approach: as P5.T4. `NativePluginList_Test` must expect `fr.natron.Merge` and no `net.sf.openfx.Merge*` IDs.
  - verify: these tests pass at the gate.
  - size: M

- [ ] M101.P5.T6 — Delete the old Merge
  - files: `Engine/Nodes/Merge/Merge.{h,cpp}`, `Engine/Nodes/Merge/MergeOperators.{h,cpp}`, `Tests/Native/NativeMerge_Test.cpp`, `Tests/Native/MergeOperators_Test.cpp`, plus the registration and references in `AppManager.cpp`, `AppInstance.cpp`, `Node.cpp`, `ParallelRenderArgs.cpp`, `EffectInstance.h` and `NodeDocumentation.cpp`
  - approach:
    - Delete the files and every reference: `git grep` for `PLUGINID_NATRON_MERGE`, `PLUGINID_OFX_MERGE`, `net.sf.openfx.Merge`, `MergeNode` and `MergeOperators` until nothing remains outside `docs/` history.
    - Optionally rename `NativeMerge.*` to `Merge.*` now that the name is free.
    - Confirm openfx-misc's Merge stays retired from the bundle (M67 - Native Core Nodes' fork PR), so the old IDs resolve to nothing. Old projects then load the missing-plugin node, which is M22 - Missing Plugin Placeholder, `todo`.
  - verify: the grep is empty and the full build and test suite pass at the gate.
  - size: M

## Phase 101.6: Docs and UAT

- [ ] M101.P6.T1 — Document the new Merge
  - files: `Engine/Nodes/README.md` (§148, §585, §615 and the tolerance table §658-676), the node's description string in `NativeMerge.cpp`, `docs/` user docs for Merge if present
  - approach:
    - Describe the inputs, operators with formulas, layer modes, alpha sources and what was dropped and why.
    - Put the HDR-safe deviation table in the user docs.
    - Remove the old Merge's README entries.
  - verify: the docs build (if the docs pipeline from M6 - Documentation Pass has a check). Review the text against the Design.
  - size: M

- [ ] M101.P6.T2 — UAT
  - files: none (user acceptance)
  - approach: hand the user a build plus a checklist:
    - over/screen/multiply on a CG multipass, in All matching and in List
    - per-layer alpha on beauty AOVs
    - per-channel alpha with an `alphaRGB` layer
    - three A inputs
    - mask and mix
    - each bbox mode
    - a GPU-resident comp, with the profile showing one region through the Merge
    - RotoPaint strokes
    - each bundled PyPlug
  - verify: the user signs off. Record the findings under `## Decisions`.
  - size: S

**Verification gate:** in one build, the full test suite passes in both scheduler modes:
- `MergeMath_Test`, `NativeMergeNode_Test` and `GpuMerge_Test` (lavapipe) pass.
- `git grep` finds no reference to the old Merge IDs or classes.
- RotoPaint and every bundled PyPlug render.
- CI is green.
- The user signs off the UAT.

## Decisions

- 2026-10-12 — **No compatibility with the old Merge** (user): old Merge IDs are deleted outright. Old projects show the missing-plugin node (M22 - Missing Plugin Placeholder).
- 2026-10-12 — **Maths are Nuke's formulas, made HDR-safe, with no legacy toggle** (user).
- 2026-10-12 — **Dropped** (user): Nuke's mask fringe and inject, video colorspace, and alpha masking (`screenAlpha`). Masking is just the base class's mask channel, invert and mix.
- 2026-10-12 — **Per-channel alpha comes from an alpha layer** whose channel *i* is the alpha of merged channel *i* (user). This fits the 4-channel layer cap.
- 2026-10-12 — **Placed after M84 - GPU Placement And Residency and before M85 - Native GPU Kernels** (user). M85 no longer ports Merge.
````

### 2026-10-12T00:34:00-04:00 — change-request
- refs: M85
- Edit the stub for M85 - Native GPU Kernels: drop **Merge** from the "cheap point ops" list (it becomes "Grade, ColorCorrect, Shuffle, Copy, Premult/Unpremult, Clamp and the like"). In the "Single-node regions" hand-off bullet, change "Porting the neighbouring keyers, morphology and Merge" to "Porting the neighbouring keyers and morphology (Merge's GPU path comes from M101 - Clean-Sheet Merge Node)". Add `M101` to its `Blocked on:` line, and set the board Deps for M85 - Native GPU Kernels to include M101 if Deps is being filled. Reason: M101 - Clean-Sheet Merge Node ships its own GPU kernel and deletes the old `MergeNode`, so porting the old node would be throwaway work (user, 2026-10-12).

### 2026-10-12T11:28:44-04:00 — change-request
- refs: none
- New milestone, next free ID is M115 (highest in use is M114). Add this board row (Deps "-", no Epic) right after M94 - Delete Unused Maintainer Skill, or wherever fits — ordering not specified by the user, it's unscheduled like M25/M29/M68:

  | M115 | Build Speed And Artifact Size | todo | - | [M115-build-speed-and-artifact-size.md](PLAN/MILESTONES/M115-build-speed-and-artifact-size.md) |

  Full milestone file content below. Context for the PM: raised in a `/cat-discuss` side session investigating why some hosts produce ~400MB AppImages and others ~100MB. Root cause (confirmed by reading `CMakeLists.txt:60-62`, `tools/ci/local/build.sh`, and the packaging scripts): `CMAKE_BUILD_TYPE` defaults to `RelWithDebInfo` (`-O2 -g`) when nothing overrides it, the `release` preset deliberately leaves it unset to match CI, and neither `tools/release/stage-bundle.sh` nor `tools/release/make-appimage.sh` ever strips debug info — so whichever build type produced the staged binaries rides straight into the shipped artifact. This milestone is scoped separately from M27 - Real Debug Build (which fixes `NDEBUG` leaking into the *debug* preset specifically) — no overlap.

  ```markdown
  # M115 - Build Speed And Artifact Size

  Full title: Faster local UAT builds, clearer build options, leaner release artifacts

  Three build presets already exist in `tools/ci/local/build.sh` (`debug`: `-O0 -g`;
  `release`: no explicit type, inherits `CMakeLists.txt`'s `RelWithDebInfo` default
  — i.e. `-O2 -g` — to match CI exactly; `fast`: explicit `Release` overridden to
  `-O1`, `-DNDEBUG`, no `-g`). Nothing currently staged into a bundle
  (`tools/release/stage-bundle.sh`, `tools/release/make-appimage.sh`) strips debug
  info, so whichever preset built the tree determines the shipped artifact's size —
  this is the confirmed cause of AppImages ranging from ~100MB to ~400MB depending
  on which build type produced them. Separate from M27 - Real Debug Build, which
  fixes a correctness bug (`NDEBUG` leaking into the debug preset) rather than
  speed or artifact size.

  ## Phase 115.1: Faster local builds for UI/UAT iteration

  - [ ] M115.P1.T1 — Trim what a UI-only edit rebuilds under the `fast` preset
    - files: `tools/ci/local/build.sh`, `CMakeLists.txt` (only if a new build option is needed)
    - approach: touch a `Gui/`-only file, rebuild with `fast`, and check ninja's
      build graph (`ninja -t targets` / `ninja -n`) for anything unnecessary
      pulled in for a UI-only change — rebuilding `Tests/`, re-fetching/rebuilding
      OFX plugin assets, or unrelated `Engine/` translation units. Cut what's safe
      to skip (e.g. gate `NATRON_BUILD_TESTS` off for `fast`) without breaking the
      preset for anyone who does need tests built.
    - verify: time a `Gui/`-only incremental rebuild under `fast` before and after;
      record the improvement in this file's `## Decisions`; `fast` still produces
      a binary that starts (`check-startup.sh`).
    - size: M

  ## Phase 115.2: Make build options discoverable and correct

  - [ ] M115.P2.T1 — Print the resolved build type and flags at the start of every `build.sh` run
    - files: `tools/ci/local/build.sh`
    - approach: before configuring, echo which preset was selected and its
      resolved `CMAKE_BUILD_TYPE` plus the actual compile flags that implies
      (optimization level, `-g` or not, `NDEBUG`/`DEBUG`) — so running `release`
      makes it obvious it ships debug info today, without reading the script.
    - verify: running `build.sh` with no args, `debug`, `release`, and `fast` each
      prints its resolved type/flags before `cmake` runs.
    - size: S

  - [ ] M115.P2.T2 — Document the three presets in `tools/ci/local/README.md`
    - files: `tools/ci/local/README.md`
    - approach: one short section per preset — what it compiles with, how long it
      roughly takes relative to the others, and when to reach for it (`fast` for
      UI/UAT checkpoints where render performance doesn't matter, `debug` for
      single-file iteration with live assertions, `release` to match CI/ship).
    - verify: README's description of each preset's flags matches
      `CMAKE_BUILD_TYPE_ARGS` in `build.sh` exactly.
    - size: S

  ## Phase 115.3: Minimize and verify release artifact size

  - [ ] M115.P3.T1 — Strip debug info when staging a bundle, regardless of build type
    - files: `tools/release/stage-bundle.sh`
    - approach: after the ldd-closure walk and before (or as part of)
      `check-relocatable.sh`, strip every staged ELF (binaries, `lib/*.so*`, Qt
      plugins, OFX bundle libraries) — e.g. `strip --strip-unneeded`, or split
      debug info out with `objcopy --only-keep-debug` into a sidecar tree that is
      never copied into the AppDir — so a `release`-preset (`RelWithDebInfo`)
      bundle is stripped at packaging time instead of at compile time, keeping
      developer builds debuggable while the shipped artifact stays lean.
    - verify: stage a `release`-preset build end to end and confirm the resulting
      AppImage is a small fraction of its unstripped size; `check-relocatable.sh`
      and `check-startup.sh` still pass on the stripped tree.
    - size: M

  - [ ] M115.P3.T2 — Re-measure AppImage size and record the before/after
    - files: none (measurement and decision log only)
    - approach: build the `release` preset, package it via the normal pipeline
      before and after Phase 115.3's stripping change, and note both sizes.
    - verify: sizes recorded in this file's `## Decisions`.
    - size: S

  **Verification gate:** all three `build.sh` presets print their resolved
  build type and flags; `tools/ci/local/README.md` documents them accurately;
  a `release`-preset AppImage built end-to-end through the normal pipeline is
  stripped and its size recorded as a clear improvement over the pre-milestone
  baseline; `check-relocatable.sh`, `check-startup.sh`, and the ctest suite
  stay green throughout.
  ```

### 2026-10-12T00:00:00Z — change-request
- refs: M96
- From the M69 - Minor Cleanup UAT (user, 2026-10-12), add this to M96 - Weighted Curve Tangents' acceptance sketch. In bezier mode a curve-editor handle has two states. **Unbroken:** the in and out handles stay aligned, and changing one handle's length changes the other's too. **Broken:** each handle's length and angle change independently of the other. Weighted handles must also change the curve, not snap back on release. The user decided M69 does not implement length; M69 instead drops its visual-only follow-the-cursor handle drawing.

### 2026-10-12T00:00:01Z — change-request
- refs: none
- A follow-up from the M69 - Minor Cleanup UAT (user, 2026-10-12): "we shouldn't be 'copying' things when the node is in single-value mode, it should be referenced." Investigate, and possibly correct, the architecture of a folded (single-value) multi-component knob. Today folding only equalizes the current value, and M69.P10.T10 makes curve-editor edits copy dimension 0's curve to every dimension. The user wants a folded knob to hold one value or curve that the dimensions reference. Candidate homes are M31 - Architectural Cleanup and M95 - Knob Cleanup And Keyer Redesign.

### 2026-10-12T00:00:02Z — info
- refs: none
- Left over after M45 - Native Tab Menu merged: `Tests/gui/m76_write.py:181,183` still look up `gt.widgets_of("NodeCreationDialog")`. That class is gone (the Tab menu is now tabtabtab, and the completer was renamed `CompleterLineEdit`), so those lines of the M76 - Native Write GUI script will no longer find the node menu. They need the `m75_read.py` treatment: drive `TabTabTabWidget`. No milestone owns this yet.

### 2026-10-13T00:00:00Z — change-request
- refs: none
- Pre-existing bug found during the M56 end build: at `QT_SCALE_FACTOR=2`, clicking directly on a curve in the curve editor doesn't select it (a click aimed at y=150 only picked `gamma.r` at y=14), so the curve hit-test likely ignores the device pixel ratio. At 1× it works. Needs a home: a task in a curve-editor milestone (M96?) or a new small fix milestone.

### 2026-10-16T02:40:00-04:00 — change-request
- refs: M110
- When M109 - GPU Region Engine Hardening was elaborated, it moved multi-plane placement candidates, which Shuffle-family nodes need to join regions, out of its scope and into M110 - Remaining Native GPU Ports. The work needs a multi-plane `GpuQuery`/`GpuRenderArgs` in `Engine/Nodes/Image/GpuRender.h`, which M126 - Runtime GPU Shaders edits. M110's stub says it is blocked on "M109's multi-plane placement". Make that a task of M110's own instead, starting after M126 merges, and add M126 to M110's Deps.

### 2026-10-18T23:30:00Z — change-request
- refs: M127
- From the gpu PM's elaboration of M107 - GPU Viewer Hand-off. M107.P3.T1 creates the shared OCIO→Vulkan helper `Engine/OcioVulkanShader.{h,cpp}`. It builds OCIO's `GPU_LANGUAGE_GLSL_VK_4_6` shader with LUTs as `GpuTexture` and dynamic properties in a UBO, and keeps a per-processor LUT texture cache keyed on `cacheID`. M127.P3.T1 should reuse that helper instead of creating `Engine/Nodes/OCIO/OcioGpuShader.*`, and M127 should add M107 to its Deps, or depend on M107.P3.T1 landing. Also stale once M107.P5.T1 lands: M116 - GPU Region Overlay's note that a region feeding the viewer is still a download.

### 2026-10-16T09:00:00Z — change-request
- refs: none
- New milestone requested (user accepted the miss in M90 - Render Path Serial Overhead): **Blur In-Graph Overhead** (E6). In-graph native Blur at UHD on 16 threads is 3.1× its bare kernel at FIR σ3 (4.70 vs 1.51 ms/Mpx) and 2.0× for IIR (6.83 vs 3.37). Profile on the 16-thread host (symbolised build) and cut the fixed per-task overhead (fill, `writeWindow` finish, planar buffer allocation) and the padded-region cost at large σ. Data: `M90-render-path-serial-overhead.md` Decisions 2026-10-16. Deps: M90.

### 2026-10-16T13:40:00Z — change-request
- refs: none
- New milestone requested (user): **Complete Local Build Environment** (E2). Make the local dev workflow self-sufficient and current, so a PM or implementer can verify GPU code, run the debug ctest PR CI runs, and package without relying on remote builds. Findings from 2026-10-16:
  - The natron-dev image has no `slangc`. `cmake/FetchSlang.cmake` downloads Slang into `<checkout>/.natron-deps` at configure time, which needs network the container may lack. On the bosley host it was seeded by hand from `/opt/slang`, and each worktree needs a symlink.
  - With the host in remote build mode, no local tree postdated M85, so `build/syntax-check.py` silently skipped every GPU translation unit. Implementers then wrote stub kernel headers by hand.
  - `build/syntax-check.py` has no `-I Tests`/`-I libs/xxhash` for test TUs (every agent adds them by hand). It takes a checkout root and base ref, not file paths, which agents keep getting wrong.
  - Remote builds (`tools/ci/remote/build.sh`) offer only `fast`/`release` (both NDEBUG), while PR CI runs debug with live asserts and a parallel ctest that hides assert text (`natron-pr-ci-debug-asserts` memory).
  - `build.sh` resets submodules, and worktree lanes need their own `fetch-assets` and `.natron-deps`.

  Scope ideas:
  - Add Slang (and anything else the build fetches at configure time) as an image layer.
  - Keep a maintained local GPU-era tree, or have syntax-check configure one on demand.
  - Fix syntax-check's include paths and CLI.
  - Add a `debug` preset to the remote build and `-V` output for failing cases.
  - A worktree bootstrap script (submodules, fetch-assets, `.natron-deps`, appimagetool wrapper).
  - Document it in `tools/ci/local/README.md`.

  Deps: none.

### 2026-10-16T14:10:00Z — change-request
- refs: none
- Bug, found by PM in PR CI (PR #64, legacy ctest, run 38894698099): `Project::saveProjectInternal` (`Engine/Project.cpp` ~:777) writes the project to a temp file named from `QDateTime::toMSecsSinceEpoch()` in the temp dir. Two processes saving in the same millisecond collide: parallel ctest did this, and so would two Natron instances. The first save fails with "device not open" and `Failed to save`. It made `ProjectSaveLoadPreservesKnobShuffleMapRows` and `...UserKnobShuffleMapRows` fail intermittently. Fix with `QTemporaryFile` or `QSaveFile` beside the target. Small; fits E2 or E7.

### 2026-10-16T16:30:00Z — change-request
- refs: none
- New milestone requested (user accepted a known-plugin-bug list in M100 - Content Node Hashing's cache-equivalence sweep): **Bundled Plugin Bug Fixes** (E3). Fix the three bundled-plugin bugs the sweep found, in our openfx-misc and openfx-arena forks, then bump the bundled assets and empty the sweep's known-plugin-bug list:
  - CImg EdgeExtend `mergeOver` assumes 4 channels and reads out of bounds when processA is off;
  - the CImg expression parser reads an uninitialised variable in CImg.h (patch at `build/ceq/cimg-undefined-variable.patch` on the bosley host);
  - fxarena Sketch uses unseeded noise, so its renders are non-deterministic; give it a fixed seed.
  
  Deps: M100.

### 2026-10-19T08:00:00Z — change-request
- refs: none
- Follow-ups from M100 - Content Node Hashing (merged d521f111a), found by PM:
  1. **A Write edited mid-render can cache a stale frame.** Off-main viewer renders that find stale input-related data abort and re-render (Engine/ParallelRenderArgs.cpp, ViewerInstance.cpp). File renders and non-abortable renders (tracker, colour sampler) are left as they were, because aborting fails the job, and a blocking main-thread wait risks deadlock. Needs a design for refreshing input-related data safely from a render thread, or keying Natron's own metadata by node hash.
  2. **A known-bug plug-in child hangs under the taskgraph scheduler.** `CacheEquivalenceKnownPluginBugTest.KnownPluginBugsStillCrash` passes only because its 60 s child watchdog fires (62.9 s in taskgraph vs 3.3 s legacy, release run on 5d5bf9e50). The segfault-handler exit deadlock is already fixed, so this is a different hang: either CImg EdgeExtend or the CImg expression plug-in hangs rather than crashing when rendered on the pool. Get a stack (eu-stack) of the child to tell plug-in UB from an engine deadlock reached after a plug-in fault. It may fold into the requested Bundled Plugin Bug Fixes milestone.
  Suggested: E14 (Architecture) or E7.

### 2026-10-16T12:30:00Z — change-request
- refs: none
- New milestone requested (user): **Native Metadata Utilities** (E3, after M77 - Native Metadata Nodes). Native versions of the OFX example metadata nodes that M77 left without a native equivalent — `org.openfx.examples.metadataCompare`, `metadataContribute` and `metadataTimeCode` — then retire the OFX ones from the bundle and the asset fetch. Also replace the three OFX example plug-ins the test suite uses as metadata-bridge fixtures (Contribute, Print, TimeCode in `build/assets/TestPlugins/`) with an in-repo test-only OFX plug-in, so the bridge keeps coverage once they're gone. Deps: M77.

### 2026-10-20T10:30:00Z — change-request
- refs: none
- Proposed follow-up milestone (E5 - GPU Compute or E6 - Render Performance), from M109 - GPU Region Engine Hardening's gate decision: cut the first-render page-zeroing cost of GPU region download destinations. A fresh 4 GiB destination (Erode 40 at 16k) waits ~100–256 ms while the kernel zeroes new pages at ~16–24 GB/s, about a third of region wall in a fresh process. Candidates: transparent or explicit huge pages for pooled `HostImageMemory` buffers, a pre-faulted and pinned destination pool reused across renders and frames, or prefaulting on an idle thread at startup. The CPU path pays the same zeroing, so judge it on fresh-process single-frame wall, Auto against Off and CPU-only against main.

- 2026-10-20 — change-request — from: gpu — refs: none
  Propose a follow-up E5 - GPU Compute milestone, after M109 - GPU Region Engine Hardening: find why GPU region downloads in fused comps run at about 6 GB/s against GpuBench's 16.7 GB/s host-import download rate on the same boot. M109.P8.T1 measured comp exposed transfer at 1.6–2.2× the GpuBench-rate floor (keying_grade, cg_multipass, defocus_retime at UHD/8K); single-node 16k/24k regions are at 0.9–1.08×. Data in `.local/m109/p8t1/analysis/final/regions.md`. Candidates: small per-member downloads, image layout or format conversion, an unpipelined last strip.

### 2026-10-21T14:00:00Z — info
- refs: none
- Pre-existing bug on main, found by M107 - GPU Viewer Hand-off's UAT and outside its scope (user: inbox note only, for /cat-plan to plan). Disconnecting a viewer's active input sets A to -1 and the viewer goes black even when another input is still connected: Read → Grade1 → Viewer input 0, `viewer.connectInput(1, Grade2)` (connecting also makes input 1 the active A, via `InspectorNode::refreshActiveInputs`), then `viewer.disconnectInput(1)` → `getAInput()` is -1. Same on main `1c0b5ab3b` and the M107 lane. Fix belongs in `Engine/NodeInputs.cpp`. Repro: `.local/m107/p6t1c/viewer_ab_zoom_repro.py` with `repro.sh` (on the gpu host).
