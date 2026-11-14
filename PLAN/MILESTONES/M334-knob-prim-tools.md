# M334 - Knob Prim Tools

Setting and updating paths and expressions on existing nodes, not only new ones. Every prim path
or pattern knob gets a selection menu, a popup tree showing current matches, an armed viewer pick,
and the tree filter doubles as a pattern editor. Builds on M232 - Scene Selection To Nodes.
Design: `PLAN/DESIGN/2026-11-04-scene-graph-design.md`.

## Scope

**In:**
- Selection menu on the knob: Replace, Add, Remove with the current selection, and Select Matches.
- Popup tree over the node's input with current matches checked, and Search Unloaded.
- Armed pick: Pick, select in viewer or panel, Enter writes, Esc cancels.
- The tree filter accepts path expressions, highlights matches live, and Apply To Node writes the expression.

**Out:**
- Generating wildcards from a selection — none for now; the filter is where wildcards get built.
- Pattern language — M52 - USD Layer-Stack Completion's `PrimSelector`.
- Engine-side matching and Search Unloaded — M331 - Deferred Pattern Matching.

**Edge cases:**
- Remove on a pattern knob whose text is an expression, not a path list: remove literal entries it can find, and otherwise append `- /path` in `SdfPathExpression`'s difference syntax.
- A path knob takes one prim from a multi-selection, the same one everywhere, as M232 - Scene Selection To Nodes settles it. Replace is the only menu action there.
- An armed pick is cancelled by closing the settings panel, deleting the node or switching viewers. Only one knob can be armed.
- The popup reads the node's input, so paths outside it are listed as "not in input" rather than dropped.
- Select Matches and the filter report unsearched unloaded branches.
- `//` recursion, not `/**`: filter help and tooltips must use USD's real syntax.
- Each write is one undo step.

**Assumptions:**
- M52 - USD Layer-Stack Completion's `PrimSelector` has reached main by the time this starts.

## Phase 334.1: On the knob

- [x] M334.P1.T1 — Selection menu on prim knobs
  - files: `Gui/KnobGuiString.{h,cpp}`, `Gui/SceneSelectionActions.{h,cpp}`, `Tests/gui/scene_knob_tools.py`
  - approach: a small menu button on flagged knobs. Replace, Add and Remove take the current viewer selection and write one undo step. Select Matches evaluates the knob through `PrimSelector` and sets the selection set in viewer and panel. Share the write path with `dropPrimPaths`.
  - verify: GUI script on USDPrune — Add appends, Remove takes out, Replace swaps, each undoable; Select Matches on `/a/*` highlights those rows.
  - size: M

- [x] M334.P1.T2 — Popup tree with current matches
  - files: `Gui/ScenePrimPickerPopup.{h,cpp}`, `Gui/KnobGuiString.cpp`, `Tests/gui/scene_prim_picker.py`
  - approach: extend M232's popup. Opens on the node's input, unloaded. Current matches are checked and their ancestors opened. Search Unloaded calls M331 - Deferred Pattern Matching's query and shows results marked as unloaded. OK writes literal paths, unless the text was an expression and nothing changed, which leaves it alone.
  - verify: GUI script — opening on `/a /b` shows both checked; OK with no change leaves an expression knob's text intact; Search Unloaded on the city finds a match inside an unloaded payload without loading it on the viewer.
  - size: M

- [x] M334.P1.T3 — Armed viewer pick
  - files: `Gui/KnobGuiString.cpp`, `Gui/SceneViewport.{h,cpp}`, `Gui/SceneGraphPanel.cpp`, `Tests/gui/scene_armed_pick.py`
  - approach: Pick puts the viewer and panel into a visible armed state for that knob. Selecting works as usual, Enter writes one undo step, Esc cancels. Show the armed knob in a viewer banner; `M326 - Legible Viewer Warning Banner` isn't a dependency, so use the existing viewer overlay text.
  - verify: GUI script — pick two prims, Enter writes both; Esc leaves the knob unchanged; deleting the node cancels the mode.
  - size: M

## Phase 334.2: Filter as pattern editor

- [x] M334.P2.T1 — Filter takes path expressions and applies them
  - files: `Gui/SceneGraphPanel.cpp`, `Gui/SceneGraphModel.{h,cpp}`, `Tests/gui/scene_filter_pattern.py`
  - approach: text beginning with `/` or containing `%` or `{` is parsed as an expression through `PrimSelector`; other text stays a name search. Matching stays incremental (8 ms slices between stage-lock releases, per the 2026-11-02 M231 - Scene Graph Panel decision) and uses `UsdObjectCollectionExpressionEvaluator`'s incremental searcher. Add `loaded` and `active` predicates through a custom predicate library. Apply To Node writes the expression to the current node's first prim pattern knob as one undo step. Invalid expressions show the parse error beside the field.
  - verify: GUI script — `//{kind:component}` lists only components and highlights in the viewer; Apply To Node writes that text; the panel reports unsearched unloaded branches; the main thread stays within the stall budget on the flat file.
  - size: L

## Phase 334.3: Select Matches on loaded prims

- [x] M334.P3.T1 — Select Matches searches loaded prims and offers to load the rest
  - files: `Gui/SceneSelectionActions.{h,cpp}`, `Gui/KnobGuiString.cpp`, `Tests/gui/scene_knob_tools.py`
  - approach: replace the private everything search with matching on the viewer's loaded stage through `matchPattern` with the viewer's evaluation context, reporting "N unloaded branches not searched". The report offers to load those payloads in the viewer (through the viewer's normal payload load request, which the user can undo by unloading), after which the search re-runs and selects inside them. Never loads anything without that action.
  - verify: GUI script — on a payload scene with nothing loaded, Select Matches selects only loaded matches and reports the unsearched count with no payload loaded; taking the load action loads the matching payloads and selects the matches inside them.
  - size: M

**Verification gate:** release gate green; GUI scripts pass under Xvfb; on one USDPrune, drag, each menu action, popup, armed pick and Apply To Node all round-trip through Undo; screenshots of the menu, popup and armed state shared before sign-off; accepted from the UAT AppImage.

## Decisions

- 2026-11-13 — P1.T1 implemented (d2f484280), unbuilt until the phase build. Select Matches uses `SceneOps::UnloadedSearch` on input 0, which loads payloads privately, so it searches everything and has no "unsearched" branches to report; matches under payloads the viewer hasn't loaded are selected in the viewer but have no panel rows.
- 2026-11-13 — Select Matches searches only what the viewer has loaded, reports unsearched unloaded branches, and lets the user load those payloads from there to select inside them (user); P3.T1 replaces the P1.T1 private everything search.
- 2026-11-13 — P2.T1 implemented (965ec29ce). `UsdObjectCollectionExpressionEvaluator` can't take a custom predicate library, so the walk uses its `PathExprEval` type with the collection predicates plus `loaded` and `active`, and that type's `IncrementalSearcher`. Expression mode tests inactive prims (so `{active:false}` finds anything) but never descends below them. Typing an expression sets the viewer selection; the message line sits under the field. `%` (collection references) is rejected by the shared parser.
- 2026-11-13 — P3.T1 first pass (e97bd1d2c) matched on the viewer's stage and polled for load completion; PM correction in progress: match the node's input under the viewer's load rules (a USDPrune's output hides its own matches), and re-search on `sceneSynced` instead of polling.
- 2026-11-13 — P3.T1 corrected (5a9ae6a87). The viewer is the active one if its scene reaches the node, else the first other viewer that does (no recency tracking exists). Full milestone gate running; the phase-1 build was cancelled as superseded.
- 2026-11-13 — Gate run 43444668767: ctest green; four GUI checks failed. Three were script bugs (a temporary panel wrapper killed its tree; the city popup case expected the first scene's children). One was a product bug: `{loaded:false}` matched inactive prims, since USD reports them unloaded; both loaded values now need an active prim, and false needs an unloaded authored payload (prims under an unloaded payload match neither). Fixed in b9d84ea4d; re-gating.
- 2026-11-13 — Script fixes for the Select Matches viewer rule; F4 (now with M401 and main) merged in (822c325d7). Full gate running with the M334 scripts plus M401's panel scripts.
- 2026-11-13 — The Select Matches failures were a product bug: `startMatchSearch` held the search only in a local variable, so it was destroyed before the background reader reported. Select Matches did nothing, and no load offer appeared. Since 5a9ae6a87, the Gui owns the search until it ends (fixed in 0d790e126). Re-gating.
- 2026-11-14 — Gate on 0d790e126: ctest green; `scene_knob_tools.py` failed two checks (Load and select kept only `/World/Plain`; the load offer came back). Product race: the re-search started once the load thread had dequeued the batch (`hasPendingStageLoads` false) but before the viewer published its new load rules. It now waits on `hasOutstandingStageLoads()` (35e85b086). Re-gating.
- 2026-11-14 — Gate green on 35e85b086: ctest and all seven GUI scripts (scene_knob_tools 76/76). F4 has since gained M330 - Graph Draw Policy; merging it hit conflicts in SceneSelectionActions, being resolved. Then PR into F4, review, re-gate. Screenshots go to F4's UAT.
- 2026-11-14 — F4 merged in (36b9d8dab). Where M330's draw policy meets the search: a search started while a policy match was pending, and Load and select's re-search after Expand Full+Load follow-up batches, could miss matches. PM call: fix here. `SceneViewport::isDrawPolicyPending()` and `drawPolicySettled()`; both searches wait for no outstanding loads and no pending policy (3348be249); the script gains a trigger-city step. Known gap: a skipped worker sync re-requested with the same rules can report settled early. PR #283 into F4 opened; Codex review and re-gate running.
