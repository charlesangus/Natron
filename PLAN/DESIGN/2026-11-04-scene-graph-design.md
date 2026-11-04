# Scene graph and deferred loading: design (2026-11-04)

Replaces the folding model of M231 - Scene Graph Panel. Decided with the user in a grilling
session. Research and evidence: `2026-11-04-scene-graph-research.md`. "(user)" marks a user
decision; the rest follows from those or from code and USD findings.

## Principles

1. **Browsing never changes what's drawn.** The tree's twirl state and the viewer's draw state are separate (user). This is a change from the M231 UAT wording, where a closed row meant a box.
2. **Loading a payload is an intentional act on a row** (user). Nothing loads because a default, a pattern or a parent's inherited state asked for it, except under an explicit Full + Load.
3. **Renders never depend on the viewer.** Draw states shape viewers only; USDRender and USDWrite get the whole scene with all payloads (user).
4. **Display state stays out of USD layers.** No custom data, nothing for USDWrite to strip.
5. **Heavy work never runs on the main thread.** Budgets below enforce this.

## The state model

Three independent things:

| State | Held by | Effect | Lifetime |
|---|---|---|---|
| **Twirl** | Each panel, by path | Which rows are listed. Twirling a payload row loads its own payload (composition only) on that panel's stage, nothing else | Carries across nodes; not saved |
| **Draw state** | Each viewer, by path | What the viewer draws | Carries across nodes by path; saved with the viewer (user) |
| **Load set** | Each stage | Which payloads are composed. Follows from twirls and draw states | The viewer's set is saved with it; closing a row never unloads, Unload is an explicit action |

### Draw states (user)

A column on each row. Unset rows inherit.

| State | Viewer draws | Payloads |
|---|---|---|
| **BBox** | One box around everything below | Stay unloaded |
| **Children** | Each direct child by its own state; unset children default to BBox | Loads only when set directly on an unloaded payload row, and only that payload |
| **Full** | All geometry below | Never loads; unloaded payloads stay as tinted BBoxes |
| **Full + Load** (working name) | All geometry below | Loads every payload below |
| **Hidden** | Nothing, no box | Stay unloaded |

- The nearest set state wins, so BBox or Hidden under a Full parent applies. A BBox or Hidden ancestor stops everything below it; deeper settings are remembered.
- Setting Full or Full + Load on a deep row switches ancestors that would block it to Children.
- Viewer keys on a picked box: ↓ Children, Shift+↓ Full, Ctrl+Shift+↓ Full + Load, ↑ BBox on the parent.
- **Unknown bounds:** an unloaded payload with no `extent` or `extentsHint` authored outside the payload draws a tinted marker at its transform, with a badge on the row. It is never measured (user). Loaded and non-payload prims get real boxes, computed from their children's extents on a worker.
- **Reveal is progressive and cancellable.** New prims reach Hydra in per-frame batches with boxes standing in, and changing the state stops the rest. The mechanism spike has to confirm this is possible; session-layer draw mode can't do it.

### Precedence

Viewer's interactive state, then graph policy, then the default. Native nodes' geometry defaults to Full. Each row shows whether its state comes from the viewer or the graph (fixing Solaris's main confusion), and Reset to Graph clears interactive state.

## Procedural control

- **Graph policy travels along scene edges beside the layer stack, on `ScenePayload`,** never inside layers.
- **USDRead:** `Initial Draw` knob with BBox (default, user), Levels, To Kind, Full, Full + Load. It applies only to the prims that USDRead brought in, one box per top-level prim by default.
- **USDExpandHierarchy:** rules of pattern and draw state, applied to the viewers below it. Patterns use deferred matching. Full + Load re-matches after each load until nothing new matches, and can be cancelled.
- **Bake to Node** writes the viewer's current states into a USDExpandHierarchy below the viewed node and clears them.
- **Viewer render delegates** (M254 - Progressive Viewer Render) follow draw states, with a Render Everything viewer toggle (user). BBox branches don't appear in the render; their boxes stay as overlay.

## Deferred pattern matching (user's idea)

- A pattern matches what its evaluator has loaded: a viewer matches its load set, USDRender and USDWrite match everything. The load set becomes an evaluation input.
- Literal paths need no matching, so they work on unloaded content.
- A load re-matches only the downstream pattern nodes whose literal prefix can reach the loaded path. New layers and the load apply together, so nothing flashes unedited.
- Zero-match warnings become "0 matches, N unloaded branches not searched". Search Unloaded is an explicit preview action on a private stage (user).
- Known limit: a box from authored bounds won't reflect edits inside an unloaded payload.

## Graph points

- A new panel follows the viewer and uses its stage. Follow Node Selection and pin are per panel, so two panels can compare graph points (user).
- Any other node gets its own stage, opened unloaded and composed on a worker. The old tree stays up greyed until ready, and recently seen stages are kept for instant return.
- The draw column always edits the linked viewer's states by path, even when the panel looks at another node. Twirls there affect only that panel's stage.

## Setting paths

Every prim path or pattern knob gets (user):

- **Drag** from tree or viewer, and create-from-selection (already built).
- **Selection menu:** Replace, Add, Remove with the current selection; Select Matches highlights what the knob matches in tree and viewer.
- **Popup tree picker** over the node's input scene, with current matches already checked.
- **Armed viewer pick:** click Pick, select in viewer or panel, Enter writes, Esc cancels.
- **Filter as pattern editor:** the tree filter takes the same path expressions as knobs, highlights matches live, and Apply To Node writes the expression.

Path knobs take one consistent prim from a multi-selection, and USDAssignMaterial's pattern knob gets flagged.

## Inspection

All on the selected prim only (user): attributes, relationships and metadata at the current frame, each naming its authoring node or file; a Composition tab of arcs; an opinion stack from strongest to weakest, where double-clicking opens the node; a Changed Here marker and filter for what the viewed node's layer touches; a read-only layer text view. The inspector takes extra tabs, so a later Geometry Spreadsheet can sit there.

## Mechanism: open

How draw states reach Hydra is decided by a spike on the heavy fixture, with numbers shown to the user (user): a per-engine Hydra filter with boxes in a GL overlay (the Katana and Gaffer approach), or session-layer draw mode (the usdview and Solaris approach, and today's code).

## Budgets (user)

On a stripped release build on a real GPU host, against generated fixtures (nothing in LFS): a 1M-prim payload city and a 1M-prim flat file. The gate fails if:

- the main thread stalls over 100 ms during open, load, twirl or reveal;
- the first boxes take over 2 s on the payload city;
- any payload file opens before it's asked for;
- orbit drops under 30 fps at 10k boxes.

Budgets may be revised once, after the spike. The user then tries a real production scene such as ALab or Moana Island at UAT.

## Delivery

Strip folding from M231 - Scene Graph Panel and M232 - Scene Selection To Nodes, merge them as a plain panel and selection tools, then build on top (user). The new milestones are exempt from the "start nothing new" hold once both are merged (user).

| Milestone | Delivers |
|---|---|
| M327 - Heavy Scene Spike | Fixtures, harness, mechanism and engine-cost numbers |
| M328 - Deferred Viewer Stage | Stage opens unloaded, heavy work off main, payload rows |
| M329 - Viewer Draw States | Draw column, viewer keys, boxes, progressive reveal, saved state |
| M330 - Graph Draw Policy | USDRead Initial Draw, USDExpandHierarchy, row origin, Bake to Node |
| M331 - Deferred Pattern Matching | Load set as evaluation input, per-context memo, Search Unloaded |
| M332 - Scene Graph Points | Per-panel graph points, stage cache, async switch |
| M333 - Prim Inspector | Inspector tabs, opinion stack, composition, Changed Here |
| M334 - Knob Prim Tools | Selection menu, popup picker, armed pick, filter as pattern |
| M335 - Viewer Render Draw States | Cycles and MoonRay viewer renders follow draw states |
| M336 - Heavy Scene Acceptance | All budgets together, then a real scene at UAT |
| M337 - Geometry Spreadsheet | Per-element table; after M54 - SOPs-Style Geometry |
