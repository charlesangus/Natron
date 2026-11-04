# Scene Graph panel: research for the redesign (2026-11-04)

Input to the M231 - Scene Graph Panel redesign. Evidence: offline docs in `.local/docs`, OpenUSD
v26.08 source (read, not executed), and a read-only audit of `milestone/m231-scene-graph-panel`
and `milestone/m232-scene-selection-to-nodes`. Nothing here was measured. Doc gaps: the offline
Solaris archive lacks the pane reference pages, and the Gaffer docs have no Hierarchy View or
Scene Inspector pages, so some UI detail rests on release notes or recollection.

## Goals (user, 2026-11-04)

1. Heavy USD scenes load quickly and navigate smoothly through deferred loading and display.
2. Expand only the sections of interest to full geometry, interactively (viewer, panel) and procedurally (node graph).
3. View and explore the hierarchy at different points in the node graph.
4. Use the hierarchy view to set paths and expressions on new and existing nodes.
5. Explore attributes, composition arcs and the like on prims.

## How the reference tools do it

| | usdview | Solaris | Katana | Gaffer |
|---|---|---|---|---|
| Unit of deferral | Payload (`--unloaded`), population mask (`--mask`) | Payload rules, population mask, layer muting | Scene graph location, lazily cooked; USD payload rules on top | Scene location; each property computed lazily |
| Collapsed/unloaded display | Nothing, unless `--bboxStandin` | Box from lofted extents, or by expansion mode (nothing / bbox / visibility) | Box from proxy/bounds or `extentsHint`; red tint if unloaded | Box from the location's bound |
| Twirl and draw | Unrelated; Load is a separate toggle | Optional coupling, plus a lock column and saved states | Decoupled: Working Sets (Include / with children / Exclude) | One state: Viewer and Hierarchy share the VisibleSet; Inclusions and Exclusions override |
| State scope | Per app | Per viewer stage, with named saved states | Global; bookmarks save in the project | Global script context; named bookmarks |
| Procedural | None | Configure Stage (edits masks on the stage), Isolate Scene | `isolatePath`, `populationMask`, Isolate, Prune; no graph-level expansion | Prune, Isolate, Encapsulate; nothing drives expansion |
| Graph point | n/a | Follows selected node by default (composes on each click); pin (recollection) | View flag vs edit flag; state carries over by path | Focus node, per-editor pin and numeric bookmarks |
| Paths on nodes | n/a | Drag, viewer reselect, popup tree, rich pattern language | Drag to CEL, Append/Replace With Selection, link expressions | Drag to PathFilter, Select Affected Objects, edit scopes |
| Inspection | Attributes, metadata, composition, layer stack | Details pane with Composition and Layer Stack tabs; flattened stage; Debug flag | Read-only attributes; USD Text View (node contribution / stage / flattened) | Scene Inspector with compare, per-cell Show History |

### Done well

- **Katana:** drawing and rendering don't depend on looking. Payload state maps directly onto USD load rules, with inheritance shown on the row. Bookmarks bundle expansion, pins and sets.
- **Solaris:** viewer-only and authored masks both exist. Unloaded prims still have boxes because extents and kind are lofted outside the payload. Isolate Scene masks only while inside the node. One parameter takes drag, viewer pick, popup tree and patterns.
- **Gaffer:** expansion is one small object the lazy scene knows nothing about. One rule across Viewer and Hierarchy. Focus plus per-editor pin makes "view this graph point" one key. Per-cell history.
- **usdview:** complete inspection tabs, directly over USD APIs.

### Done poorly

- **Katana:** one global expansion state; two parallel trees (Geolib and USD); no documented Expand All guard; no per-opinion provenance.
- **Solaris:** the two mask kinds look alike and behave very differently; following selection composes on each click; a population mask hides prims from edits too; per-node layer cost.
- **Gaffer:** no guard on full expansion; one global VisibleSet, so views can't differ; no payload semantics; USD composition is flattened away.
- **usdview:** any resync, including every Load, rebuilds the tree; Find and stats walk the whole stage; no graph.

## USD facts that constrain the design (v26.08)

- **Only payloads and population masks defer composition.** `Open(LoadNone)` still composes all namespace outside payloads. A flat file with no payloads can only defer what reaches Hydra.
- **`Load` recomposes** and sends resyncs; `SetLoadRules` and `SetPopulationMask` recompose from root. Batch with `LoadAndUnload`.
- **Unloaded payload contents are unknowable.** No children, no pattern matches, no composition arcs. A box without loading needs `extent` or `extentsHint` authored outside the payload.
- **Draw mode under the scene-index path (the default)** uses the prim's own extent only, never its children, and saves no composition. Editing it means editing the stage.
- **No cheap per-engine exclusion in the public API.** `excludedPaths` is constructor-only and `rootPath` no longer lightens anything. `HdSceneIndexPluginRegistry::RegisterSceneIndexForRenderer` receives a `renderInstanceId`, so a per-engine filter looks possible, keyed on an undocumented string (inference).
- **Invisible prims still pay** first sync, so visibility is not a lightening tool. `active=false` removes descendants but is a stage-wide resync.
- **A second stage shares only opened `SdfLayer`s.** Each stage has its own `PcpCache`, so its full non-payload composition is recomputed. Muting is cheap but shows one graph point at a time and mutates the stage.
- **Inspection APIs all exist:** `GetPrimIndex`, `GetPrimStackWithLayerOffsets`, `GetPropertyStack`, `GetResolveInfo`, `UsdPrimCompositionQuery` (recomputes on every construction; selection only).
- **`SdfPathExpression`:** recursion is `//`, not `/**`. `{kind:component}` works. There is no `active` or `loaded` predicate; add them through a custom predicate library. It sees composed prims only.

## What the current branches do

- **Deferred loading isn't deferred.** The viewer's stage starts LoadAll, so all payloads load on the first sync. Bounds are measured with children loaded, then unloaded. Query and picker stages are LoadAll, so a pattern node or `USDExpandHierarchy` loads what it folds.
- **Browsing edits the scene.** A twirl edits a session layer and load rules on the main thread under a blocking lock, and recomposes.
- **Fold truth is scattered:** customData in scene layers (which `USDWrite` must strip), overrides, budget and reopened lists, the session layer and mirrored rows, reconciled in up to 8 passes.
- **Heavy work on main:** file closure walk, framing, count, guides, twirls, attribute reads, with blocking waits on sync.
- **Never tested on heavy data:** fixtures are about 1 KB, no GUI test mentions payloads, and the gate's fold perf numbers were never recorded.
- **Missing:** no relationships, metadata, arcs or layer stack. The filter can't find unloaded content. A path knob takes the first path on drag and the last on Set Selection; `USDAssignMaterial` isn't flagged. No path expressions until M52 - USD Layer-Stack Completion lands.

**Salvageable:** `SceneGraphModel`, stage-source abstraction, drag and knob hint, attribute view and `authoringNodeOf`, filter, follow/pin UI, picker, selection actions, Tab filtering, Wrap mode.

**Tied to folding:** `SceneStage` fold API, viewport fold state, panel mirroring, USDRead Open Mode, `USDExpandHierarchy`, the `USDWrite` strip, prim budget, `SceneFold_Test`, `scene_graph_fold.py`.

## Ideas that improve on all four

1. **Progressive, cancellable reveal.** Hand new prims to Hydra in per-frame batches, boxes standing in until geometry arrives. Expand All can't freeze the viewer, and closing the row stops it. None of the four documents a guard.
2. **Opinion to node.** Every node here emits one layer, so every opinion in a stack maps to a node, and "what this node changed" is a read of its layer. Highlight it in the tree.
3. **Demand-loaded pattern evaluation.** A query stage loads only the literal prefix of a pattern, so `/city/block3//car*` loads block3. Results never depend on viewer state and renders stay correct, without LoadAll.
4. **Bake to Node.** Turn the current interactive expansion into a procedural node with one action.
5. **Visible state origin.** Rows show whether their state comes from the viewer or the graph, fixing Solaris's main confusion.
6. **Knob round trip.** A pattern highlights its matches live in tree and viewer, and reports unloaded branches it could not search. The tree filter takes the same language, so a filter becomes a knob value.
7. **Open off-lock.** Open payload layers on a worker before taking the stage lock, so `Load` holds the lock only for composition and navigation continues.
8. **Per-viewer state,** which Katana and Gaffer can't do.
