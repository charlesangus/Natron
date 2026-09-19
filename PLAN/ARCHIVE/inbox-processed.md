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
