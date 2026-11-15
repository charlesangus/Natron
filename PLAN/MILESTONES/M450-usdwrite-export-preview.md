# M450 - USDWrite Export Preview

USDWrite's panel shows what the export will do before anyone renders it. An embedded tree lists the prims the written files hold, in path hierarchy, with three columns. **Spec** is def, over, or none for a prim that only context authors. **File(s)** names the output files that hold it: root, `_layerN` sidecars, and M449 - USD Layer Save Paths' region files. **Node Layer(s)** names the nodes that author it. Prims that exist only in context (M448 - USD Layer Break) are greyed out with an empty File cell. A file filter narrows the tree to one output, and read files show in it as external sublayers. Clicking a node cell selects and centres that node in the Node Graph. Selecting a prim row selects it in the scene, so the viewer highlights it. The preview refreshes live, off the UI thread, while the panel is open. An export that would fail shows an error row at the top of the tree with the offending nodes marked red. USDWrite also shows the same error as a persistent node error before anyone renders. Last member of F27 - USD Layer Authoring.

Both the preview and the write run on one **export plan**: `writeFrameRange`'s layout logic, extracted so it can run without writing. Then they can never disagree.

## Scope

**In:**
- An author-node identity on every stack entry.
- An export-plan function that `writeFrameRange` uses, without changing its behaviour.
- A spec-attribution pass over the plan.
- A pre-render node error.
- The preview knob and its GUI: tree, columns, file filter, context greying and error row.
- Live off-thread refresh while visible, node and prim interaction, docs, ctest coverage and one Xvfb GUI script with screenshots.

**Out:**
- Per-file layer metadata in the preview: Unconfirmed, see Assumptions.
- Showing the prims inside read files (external sublayers). They are listed as files, not walked.
- Editing anything from the preview: it is navigation only.
- Live-texture and projection-bake previews: the plan skips both, since they are expensive and add no prim specs worth showing.
- A dockable or standalone preview panel: no milestone.

**Edge cases:**
- A prim with specs in several files lists them all, comma-separated in stack order, strongest first. Spec shows `def` if any written file defines it, otherwise `over`.
- A prim authored by both context and written layers is shown normally. Only prims with no written spec are greyed out.
- In Flattened mode the plan has one file. Node attribution still comes from the non-context entries that feed the flatten.
- Referenced stacks written as sidecars appear as files in the filter. Their prims are listed under their own paths, as the sidecar holds them.
- The panel is closed: no tree is computed. The plan-only error check still runs on input and knob changes, so the node error stays current.
- An upstream change while a computation is running cancels it and starts again. The tree keeps showing the last result, marked as updating, and never blocks the UI.
- A node named in a cell has since been deleted: clicking it does nothing, and the next refresh removes it.
- Large scenes: no size cap. The tree model is lazy, fetching children on expand, so the first paint stays fast.

**Assumptions:**
- Unconfirmed: the preview does not list per-file metadata (upAxis, comment, custom data).
- `SceneViewport::setSelectedPrims` (`Gui/SceneViewport.h`), or the selection path M232 - Scene Selection To Nodes built on it, is the right hook for prim selection. The freshness check confirms it.
- `NodeGraph::centerOnItem` (`Gui/NodeGraph.h`) centres a node.

## Phase 450.1: Export plan

- [ ] M450.P1.T1 — Author node on every stack entry
  - files: `Engine/Nodes/Scene/ScenePayloadImpl.h`, `Engine/Nodes/Scene/SceneOps.h`/`.cpp` (`SceneLayerEdit`, `appendLayer`), `Engine/Nodes/Scene/SceneLayerMemo.h`/`.cpp`, `Tests/Scene/USDMergePrune_Test.cpp`
  - approach: add an author identity to `SceneLayerEntry`: a weak node pointer plus the node's fully qualified script name, for display after deletion. Every node layer reaches the stack through `SceneLayerEdit` and `appendLayer`, so set it there from the memo's owning node rather than in each node. `appendEntry` copies it. It is not part of the hash.
  - verify: unit test. In Read → USDPrune → USDLight, each entry names its node, and the entries kept through a USDMerge keep their author.
  - size: M

- [ ] M450.P1.T2 — Extract the export plan from `writeFrameRange`
  - files: `Engine/Nodes/Scene/USDWrite.h`/`.cpp`, `Engine/Nodes/Scene/USDWriteExportPlan.h`/`.cpp` (new), `Tests/Scene/USDWrite_Test.cpp`
  - approach: move the layout part of `writeFrameRange` into `planExport(scene, settings) → ExportPlan`. The settings are output path, export mode, `stripLayerBreaks`, `saveCollisions` and path mode. The layout part covers stack items, context skipping, regions, the run and sidecar layout, referenced stacks and the collision and overwrite checks. `ExportPlan` lists the written files (path, source entries, external sublayers) and the errors, each error naming its nodes. It does no file I/O, no live-texture export and no projection bake. `writeFrameRange` then calls `planExport` and writes the result. Behaviour must not change: same files, same names, same errors.
  - verify: every existing `USDWrite_Test` case, plus the M448 and M449 tests, passes unchanged. A new test checks that `planExport` on M449.P2.T1's graph lists the same file set that writing produces, and writes nothing.
  - size: L

- [ ] M450.P1.T3 — Spec attribution over a plan
  - files: `Engine/Nodes/Scene/USDWriteExportPlan.h`/`.cpp`, `Tests/Scene/USDWriteExportPlan_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: `describeExport(scene, plan) → ExportPreview`. For each entry, written or context, it traverses prim specs (`primSpecPaths`) and records the prim path, the entry's author node, the destination file (or none for context), and the specifier. Entries are folded per prim, and the rows are sorted by path for a tree. External files are listed separately. It is cancellable via a flag checked per entry.
  - verify: unit test with the M448 lighting graph. The pruned prim is `over` in the root, authored by USDPrune. The light is `def`, authored by USDLight. The fixture-only prims are greyed (no file). Setting the cancel flag stops the pass early.
  - size: M

## Phase 450.2: Preview and errors

- [ ] M450.P2.T1 — Pre-render node error from the plan
  - files: `Engine/Nodes/Scene/USDWrite.h`/`.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: on an input change, or a change to any knob the plan reads, run `planExport` off the main thread, debounced and superseding any run in progress. Follow the async pattern the scene nodes already use for upstream queries (the query-stage pool used by USDVariant's pick lists, M52). Post the result back. Set a persistent error message from the plan's errors, and clear it when they are gone. Render still fails on the same errors as before.
  - verify: test. Wiring two regions to the same save path under Error sets USDWrite's persistent error without a render, and fixing the path clears it.
  - size: L

- [ ] M450.P2.T2 — Preview knob and tree widget
  - files: `Engine/KnobTypes.h`/`.cpp` (or a new `Engine/KnobExportPreview.h`/`.cpp`), `Engine/KnobFactory.cpp`, `Gui/KnobGuiExportPreview.h`/`.cpp` (new), `Gui/ExportPreviewModel.h`/`.cpp` (new), `Gui/KnobGuiFactory.cpp`, `Gui/CMakeLists.txt`/`Engine/CMakeLists.txt` as needed, `Engine/Nodes/Scene/USDWrite.cpp` (adds the knob on its own "Preview" page), `Engine/Nodes/Scene/docs/USDWrite.md`
  - approach: a value-less, unserialized knob whose GUI is a `QTreeView` over a lazy `QAbstractItemModel` built from `ExportPreview`. It has columns Prim, Spec, File(s) and Node Layer(s). Context-only rows are greyed. A file combo above the tree ("All Files", each written file, external files greyed) filters it. Error rows from the plan head the tree, and the offending node cells show red. While the widget is visible, upstream or knob changes trigger `planExport` plus `describeExport` off the UI thread (`QtConcurrent` with a `QFutureWatcher`, as `Gui/GuiApplicationManager.cpp` uses). A superseded run is cancelled, and the old tree stays up, marked "Updating…", until the new result arrives. Hidden widgets compute nothing. The widget fits a 1920×1080 property panel.
  - verify: a Gui unit test or GUI-script assertion that the model built from the M448 lighting graph has the expected rows, columns and greyed rows, and that the filter narrows to one file. Manual: no UI stall while a heavy scene recomputes (checked at UAT).
  - size: L

- [ ] M450.P2.T3 — Preview interaction
  - files: `Gui/KnobGuiExportPreview.cpp`, `Gui/ExportPreviewModel.cpp`
  - approach: clicking a Node Layer(s) cell selects that node and centres it in its Node Graph (`NodeGraph::centerOnItem`). A cell naming several nodes opens a small menu to pick one. Selecting prim rows sets the scene prim selection through the path M232 - Scene Selection To Nodes uses (`SceneViewport::setSelectedPrims` or its wrapper), so the viewer and Scene Graph panel highlight them. A deleted node's cell does nothing.
  - verify: covered by P2.T4's GUI script.
  - size: M

- [ ] M450.P2.T4 — GUI script and screenshots
  - files: `Tests/gui/m450_export_preview.py`, GUI test registration
  - approach: under Xvfb at 1920×1080, build M449.P2.T1's graph plus a break, and open USDWrite's panel. Wait for the preview, then assert the row count and the File(s) cells for three known prims. Click a node cell and assert that node is selected. Select a prim row and assert the viewer's selected prims. Make a collision and assert the error row and the node error. Save screenshots of the preview in normal and error states.
  - verify: the script passes in the remote `--gui` gate, and the screenshots go to the user before sign-off.
  - size: M

**Verification gate:** CI green, including the export-plan tests (USDWrite output unchanged by the extraction) and `m450_export_preview.py` under `--gui`. Preview screenshots are shared with the user and approved. F27 - USD Layer Authoring's UAT covers the preview on a real scene: live refresh without stalls, navigation, and the error display.

## Decisions

- 2026-11-15 — The preview is a prim tree with Spec, File(s) and Node Layer(s) columns, with context prims greyed and a file filter (user, grill: layers and files as columns).
- 2026-11-15 — Live refresh off the UI thread while the panel is open (user, grill; briefly chose a Refresh button, then reverted).
- 2026-11-15 — Node cells jump to the node; prim rows select in the scene (user, grill).
- 2026-11-15 — Plan errors show inline and as a persistent USDWrite node error before render (user, grill).
- 2026-11-15 — Preview and write share one extracted `planExport`, so the preview can never disagree with the write (planner).
