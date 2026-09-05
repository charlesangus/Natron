# Milestone 20: 3D node vocabulary and HydraRender

"M-Scene-2" in the design doc: the Nuke-14-inspired, deliberately small v1 node
vocabulary over M19's `SceneOps`, plus the two remaining consumers —
`HydraRender` (scene → Image planes) and `WriteScene`. Requires M19's gate.
The vocabulary is a cap, not a floor (design doc, "Scope gravity"); additions
happen in named milestones with the design doc amended first.

All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Scene/`, authoring
one anonymous layer each via `SceneOps`, memoized per M19.P1.T3.

## Phase 20.1: Create nodes

- [ ] M20.P1.T1 — `Camera3D` and `Prim3D`
  - files: `Engine/Nodes/Scene/Camera3D.cpp`, `Engine/Nodes/Scene/Prim3D.cpp`, CMake source list
  - approach: `Camera3D` authors a `UsdGeomCamera` prim (focal length, aperture, clipping, transform — animated knobs as time samples per M19's time policy). `Prim3D` authors empty xforms / simple prims for layout scaffolding. Both take an optional scene input (append to stack) or act as scene sources.
  - verify: unit tests — composed stage contains the camera with knob-matching attributes; Viewport3D look-through of a `Camera3D` matches its knob values.
  - size: M

- [ ] M20.P1.T2 — `Light3D`
  - files: `Engine/Nodes/Scene/Light3D.cpp`, CMake source list
  - approach: one node, type knob selecting the UsdLux prim (distant, sphere, rect, dome; dome takes a texture path). Intensity/exposure/color knobs map to UsdLux attributes directly — no native light model.
  - verify: unit test — each type authors the right UsdLux prim; a lit sphere renders visibly differently per light type in `HydraRender` (M20.P3.T1) once it lands.
  - size: M

## Phase 20.2: Edit nodes

- [ ] M20.P2.T1 — `Transform3D` with viewport gizmo
  - files: `Engine/Nodes/Scene/Transform3D.cpp`, `Gui/Viewport3D.cpp` (gizmo drawing + drag → knob writeback), CMake source list
  - approach: xform override on a prim pattern (path or wildcard match), authored as the strongest opinion in the node's layer. Viewport gizmo: translate/rotate/scale handles drawn in `Viewport3D` when a `Transform3D` is selected; drags write back to knobs (undoable through the existing knob undo stack).
  - verify: unit test — override composes over the input's xform; manual — gizmo drag moves the prim and creates one undo entry.
  - size: L

- [ ] M20.P2.T2 — `Merge3D` and `Prune3D`
  - files: `Engine/Nodes/Scene/Merge3D.cpp`, `Engine/Nodes/Scene/Prune3D.cpp`, CMake source list
  - approach: `Merge3D` — layer-stack union (Nuke GeoMerge "merge layers" mode; duplicate/flatten modes deferred to a future amendment), multi-input, ordering = input order. `Prune3D` — deactivation/visibility opinions by prim pattern.
  - verify: unit tests — merged stack composes both inputs' prims with the documented strength order; pruned prims are inactive on the composed stage.
  - size: M

- [ ] M20.P2.T3 — `Material3D`
  - files: `Engine/Nodes/Scene/Material3D.cpp`, CMake source list
  - approach: authors a `UsdPreviewSurface` material (diffuse/metallic/roughness/emissive knobs; texture file inputs for each) and binds it to a prim pattern. MaterialX explicitly out of scope for v1.
  - verify: unit test — material prim + binding present on composed stage; manual — shaded Viewport3D shows the material.
  - size: M

## Phase 20.3: Consumers

- [ ] M20.P3.T1 — `HydraRender` node: scene + camera → Image
  - files: `Engine/Nodes/Scene/HydraRender.cpp`, `Engine/Nodes/Scene/HydraRenderImpl.cpp` (AOV/engine plumbing), CMake source list
  - approach: scene input + camera path knob → renders through a Hydra render delegate into Natron Image planes: color, depth, and prim-ID AOVs. Delegate selectable — Storm ships; others discovered via plugInfo. **Output kind: image** — this node is the scene→2D bridge, so its result flows through the normal `renderRoI` path, image cache included. Needs a GL context for Storm: reuse the Viewport3D context-creation path; without GL 4.5 the node fails its render with a clear error (2D-only pipelines unaffected).
  - verify: render a reference scene to PNG in a GUI/GL-capable environment and diff against a committed reference; depth and prim-ID planes present and plausible (depth monotonic with distance).
  - size: L

- [ ] M20.P3.T2 — `WriteScene`
  - files: `Engine/Nodes/Scene/WriteScene.cpp`, CMake source list
  - approach: exports the input payload — flattened (single-layer bake) or layered (preserve the stack as sublayers) — to `.usd`/`.usda`/`.usdc`.
  - verify: round-trip test — node-built scene → WriteScene → ReadScene composes an identical prim tree (diff via `UsdStage::ExportToString`).
  - size: S

- [ ] M20.P3.T3 — Scene vocabulary integration test in CI
  - files: `tests/`, reference `.usda` assets
  - approach: headless graph — ReadScene → Transform3D → Merge3D(+ Prim3D/Light3D/Material3D branch) → Prune3D → WriteScene — asserting on the exported composition (prims, xforms, bindings, deactivations). HydraRender is excluded from CI (no GL under Xvfb); its reference-render check runs at the gate on real hardware.
  - verify: test green in `build-and-test`; each node's authored opinions visible in the exported layer stack.
  - size: M

**Verification gate:** CI green including M20.P3.T3; on real hardware, a scene built purely from create/edit nodes renders through `HydraRender` to an image matching the committed reference, and the same scene round-trips through `WriteScene`/`ReadScene`; gizmo manipulation works with undo; pre-existing ctest suite green.
