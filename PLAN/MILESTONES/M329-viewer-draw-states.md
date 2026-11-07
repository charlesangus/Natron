# M329 - Viewer Draw States

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Each viewer holds a set of draw states by prim path, edited from a column on the tree and keys
in the viewer, and applied to Hydra by the mechanism M327 - Heavy Scene Spike settles. Boxes,
markers, progressive reveal and saving with the viewer live here. State model and rules:
`PLAN/DESIGN/2026-11-04-scene-graph-design.md`.

Blocked on: M327 - Heavy Scene Spike's mechanism decision, which determines how pruning, boxes, picking and reveal are built.

Acceptance sketch:
- The draw column shows BBox, Children, Full, Full + Load and Hidden, with inherited values visibly different from set ones; twirling a row changes nothing in the viewer.
- Setting Full on a deep row switches blocking ancestors to Children, and siblings stay as boxes.
- Viewer keys on a picked box work: ↓ Children, Shift+↓ Full, Ctrl+Shift+↓ Full + Load, ↑ BBox on the parent.
- Full + Load at the root of the payload city reveals progressively, the viewer stays navigable, and changing the state stops the rest.
- Reopening a saved project restores the viewer's draw states and load set progressively, and the load can be cancelled.

Notes from M327 - Heavy Scene Spike (2026-11-07), see `PLAN/DESIGN/2026-11-07-heavy-scene-spike.md`:
- The mechanism is the per-engine Hydra filter with a GL box overlay (user). Port it and the harness from `spike/m327-hydra-filter`.
- Add at elaboration: per-change frame cost at full load (about 1.1 s at 1M against the kept 100 ms budget), adaptive reveal batches, a real first-boxes signal, markers, picking on the overlay.
- Check why Lobby's Geometry row changed no pixels at 1.1% frame coverage under either mechanism, and whether a pruned row should hide what Lobby's yellow dots show.

## Scope

**In:**
- `SceneDrawStates`, a small per-viewer object keyed by path: pure logic with effective-state resolution, serialised with the viewer.
- The draw column on the tree. It always edits the linked viewer's states by path, even when the panel looks at another node.
- Applying effective states to Hydra through the chosen mechanism, with progressive batches where that mechanism allows it.
- Boxes computed from children's extents on a worker, for loaded and non-payload prims. Tinted boxes for unloaded payloads with authored bounds, tinted markers without.
- Picking a box selects its prim. A row badge marks unknown bounds.
- Viewer keys, rebindable in the 3D viewer's shortcut group.
- Reset to Graph.

**Out:**
- Graph policy, USDRead's knob, USDExpandHierarchy, row origin and Bake to Node — M330 - Graph Draw Policy. Until then native geometry and USDReads default to Full.
- Viewer render delegates — M335 - Viewer Render Draw States.
- Measuring unknown bounds: never (user).
- Named bookmarks of draw states — none for now (user chose saved state plus Bake to Node).

**Edge cases:**
- A path that doesn't exist at the viewed node is ignored but kept, so returning restores it.
- A BBox or Hidden ancestor stops everything below; deeper states are remembered.
- A prim whose box can't be computed yet shows its marker until the worker finishes.
- An animated subtree's box is recomputed on a worker when the frame changes; the old box stands until it's ready.
- Selection inside a boxed branch selects in the tree; the viewer highlights the nearest drawn ancestor's box.
- A saved state naming a payload whose file is now missing: row badge and USD error, and the rest restores.

**Assumptions:**
- Rendering 10k boxes meets the 30 fps budget with the filter's overlay: 51-60 fps measured in M327 - Heavy Scene Spike.
