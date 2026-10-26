# 3D MVP survey — after M19/M20 UAT (2026-10-26)

The user ran UAT on M19 - USD/Hydra Foundation and M20 - 3D Node Vocabulary and called them
a good bare-bones proof of concept. They asked what basic 3D functionality is still missing
for a comp-3D MVP, measured against Nuke (classic and USD-based 14+) and Houdini Solaris.
This doc records the survey and the grill outcomes that shaped M226–M235. Sources: a code
survey of `milestone/m20-3d-node-vocabulary` and a web survey of Foundry/SideFX/Gaffer docs.
The web pages could not be fetched directly, so the findings come from search summaries.

## What a comp needs (minimum)

1. Import a tracked camera.
2. Put cards and basic primitives in a scene, in a hierarchy you can reason about.
3. Project a 2D image through a camera, live.
4. Put live node-graph images on geometry.
5. Render the scene back to 2D (USDRender — done).
6. See, navigate, inspect and pick things in the viewer, and turn picks into node edits.

Nuke covers all six natively. Solaris covers rendering and inspection well but has no comp
projection workflow.

## Where the fork stood at M20

| Area | State at M20 | Gap → milestone |
|---|---|---|
| File picker filter | `*` matches nothing (M16 regression: `FileSystemModel::isAcceptedByRegexps` rejects all when the only token is `*`) | fixed in M20 via inbox |
| Right-click in 3D view | menu eats the release; the RMB drag state sticks and later moves dolly | fixed in M20 via inbox |
| Hierarchy | flat: create nodes at `/<Name>`, USDTransform edits by pattern, Merge is a sublayer union | M226 - Scene Hierarchy From Topology |
| Up axis | follows stage metadata; created nodes author none (Y-up, 0.01 m) | M226 |
| Primitives | none (USDPrim = xform/scope only) | M227 - USD Primitive Nodes |
| Navigation | hard-coded LMB tumble; F = frame all; single selection; fixed clip | M228 - 3D Viewer Navigation |
| Camera/light display | Storm draws no guides; nothing drawn by us | M229 - 3D Viewer Guides And Display |
| Gizmo | only for the USDEditTransform whose panel is open | M230 - 3D Transform Widgets |
| Scene graph | no tree widget anywhere | M231 - Scene Graph Panel |
| Selection → nodes | `selectedPrimChanged` connected to nothing | M232 - Scene Selection To Nodes |
| Live textures | file paths only | M233 - Live Node Textures (split from M53) |
| Projection | planned as UV bake only (M53) | M234 - Camera Projection Nodes |
| Tracked cameras | USDRead reads `.abc`/`.usd` cameras | M235 - USDCamera From File |
| Path expressions, schema edits, references, variants | planned | M52 - USD Layer-Stack Completion (now in the MVP) |

## Reference behaviour

- **Nuke 14+ (USD system).** A node's Path knob defaults to `/{nodename}`. GeoTransform and the other modifiers *edit* prims through a Mask knob (default "last modified") and never parent. GeoMerge/GeoScene combine branches, with the stronger input winning collisions. The Scene Graph panel supports drag-to-knob, and knobs have an eyedropper. Project3DShader and GeoProjectUV handle projection.
- **Solaris.** Edit LOPs work by prim pattern. A viewport transform tool creates an Edit LOP from the selection. The Scene Graph Tree can write a selection into a pattern. Graft Stages parents by flattening.
- **Gaffer.** Group and Parent build hierarchy, while Transform edits through a filter. The graph topology mirrors the hierarchy through Group. This is the model closest to what the user wants.

## Decisions from the grill (user)

- The hierarchy follows the graph topology. `USDXformPrim`/`USDScopePrim` re-parent their (multi) inputs, with optional auto-rename. `USDTransform` becomes `USDEditTransform`, `USDPrim` is retired, create nodes have no scene input, and USDRead keeps the file's roots. See `PLAN/DECISIONS/2026-10-26-scene-hierarchy-from-topology.md`.
- Up axis and units are a project setting authored by created nodes.
- Primitives are separate nodes authoring UV'd meshes, with no image input.
- Live textures are split out of M53 into the MVP.
- Projection gets both nodes: a live glslfx `USDProject3D` and a portable `USDProjectUV`.
- Scene Graph panel: tree, icons, search, two-way selection sync, attribute view with the authoring node, and drag-to-knob. Katana-style folding shows a folded branch as a bbox, and the tree starts expanded below a prim budget.
- Selection → nodes: a picker on path knobs, set-on-current-node, create-from-selection through a kind-filtered tab menu, and transform drags that create edits.
- Viewer: guides and framing, a look-through lock with clip planes, ortho views and display toggles, Houdini-style transform shortcuts, navigation presets (Nuke default, plus Maya/Blender/Houdini) rebindable in the shortcut editor, and create-camera-from-view. The gizmo target policy has three variants behind a preference.
- Tracked cameras: USD/ABC through USDRead is enough, plus USDCamera reading a camera from a file. FBX and `.chan` are out.
- M52 is part of the MVP. It follows M226 and otherwise runs wherever it fits.
