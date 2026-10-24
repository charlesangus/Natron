# M20 - 3D Node Vocabulary

Full title: 3D node vocabulary and USDRender

"M-Scene-2" in the design doc: the Nuke-14-inspired, deliberately small v1 node
vocabulary over M19's `SceneOps`, plus the two remaining consumers —
`USDRender` (scene → Image planes) and `USDWrite`. Requires M19's gate.
The vocabulary is a cap, not a floor (design doc, "Scope gravity"); additions
happen in named milestones with the design doc amended first.

All nodes are `NativeEffectBase` subclasses in `Engine/Nodes/Scene/`, authoring
one anonymous layer each via `SceneOps`, memoized per M19.P1.T3.

## Phase 20.1: Create nodes

- [ ] M20.P1.T1 — `USDCamera` and `USDPrim`
  - files: `Engine/Nodes/Scene/USDCamera.cpp`, `Engine/Nodes/Scene/USDPrim.cpp`, CMake source list
  - approach: `USDCamera` authors a `UsdGeomCamera` prim (focal length, aperture, clipping, transform — animated knobs as time samples per M19's time policy). `USDPrim` authors empty xforms / simple prims for layout scaffolding. Both take an optional scene input (append to stack) or act as scene sources.
  - verify: unit tests — composed stage contains the camera with knob-matching attributes; 3D-view look-through of a `USDCamera` matches its knob values.
  - size: M

- [ ] M20.P1.T2 — `USDLight`
  - files: `Engine/Nodes/Scene/USDLight.cpp`, CMake source list
  - approach: one node, type knob selecting the UsdLux prim (distant, sphere, rect, dome; dome takes a texture path). Intensity/exposure/color knobs map to UsdLux attributes directly — no native light model.
  - verify: unit test — each type authors the right UsdLux prim; a lit sphere renders visibly differently per light type in `USDRender` (M20.P3.T1) once it lands.
  - size: M

## Phase 20.2: Edit nodes

- [ ] M20.P2.T1 — `USDTransform` with viewport gizmo
  - files: `Engine/Nodes/Scene/USDTransform.cpp`, `Gui/SceneViewport.cpp` (gizmo drawing + drag → knob writeback), CMake source list
  - approach: xform override on a prim pattern (path or wildcard match), authored as the strongest opinion in the node's layer. Viewport gizmo: translate/rotate/scale handles drawn in the Viewer's 3D view when a `USDTransform` is selected; drags write back to knobs (undoable through the existing knob undo stack).
  - verify: unit test — override composes over the input's xform; manual — gizmo drag moves the prim and creates one undo entry.
  - size: L

- [ ] M20.P2.T2 — `USDMerge` and `USDPrune`
  - files: `Engine/Nodes/Scene/USDMerge.cpp`, `Engine/Nodes/Scene/USDPrune.cpp`, CMake source list
  - approach: `USDMerge` — layer-stack union (Nuke GeoMerge "merge layers" mode; duplicate/flatten modes deferred to a future amendment), multi-input, ordering = input order. `USDPrune` — deactivation/visibility opinions by prim pattern.
  - verify: unit tests — merged stack composes both inputs' prims with the documented strength order; pruned prims are inactive on the composed stage.
  - size: M

- [ ] M20.P2.T3 — `USDMaterial`
  - files: `Engine/Nodes/Scene/USDMaterial.cpp`, CMake source list
  - approach: authors a `UsdPreviewSurface` material (diffuse/metallic/roughness/emissive knobs; texture file inputs for each) and binds it to a prim pattern. MaterialX explicitly out of scope for v1.
  - verify: unit test — material prim + binding present on composed stage; manual — the shaded 3D view shows the material.
  - size: M

## Phase 20.3: Consumers

- [ ] M20.P3.T1 — `USDRender` node: scene + camera → Image
  - files: `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/HydraRenderImpl.cpp` (AOV/engine plumbing), CMake source list
  - approach: scene input + camera path knob → renders through a Hydra render delegate into Natron Image planes: color, depth, and prim-ID AOVs. Delegate selectable — Storm ships; others discovered via plugInfo. **Output kind: image** — this node is the scene→2D bridge, so its result flows through the normal `renderRoI` path, image cache included. Needs a GL 4.5 context for Storm: in the GUI it uses a context shared with the global 4.5 compatibility context (M19.P2.T8); headless (NatronRenderer, ctest), which has no GL requirement, it creates its own offscreen context, and without GL 4.5 there the node fails its render with a clear error (2D-only pipelines unaffected).
  - verify: render a reference scene to PNG in a GUI/GL-capable environment and diff against a committed reference; depth and prim-ID planes present and plausible (depth monotonic with distance).
  - size: L

- [ ] M20.P3.T2 — `USDWrite`
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, CMake source list
  - approach: exports the input payload — flattened (single-layer bake) or layered (preserve the stack as sublayers) — to `.usd`/`.usda`/`.usdc`.
  - verify: round-trip test — node-built scene → USDWrite → USDRead composes an identical prim tree (diff via `UsdStage::ExportToString`).
  - size: S

- [ ] M20.P3.T3 — Scene vocabulary integration test in CI
  - files: `tests/`, reference `.usda` assets
  - approach: headless graph — USDRead → USDTransform → USDMerge(+ USDPrim/USDLight/USDMaterial branch) → USDPrune → USDWrite — asserting on the exported composition (prims, xforms, bindings, deactivations). USDRender is excluded from CI (no GL under Xvfb); its reference-render check runs at the gate on real hardware.
  - verify: test green in `build-and-test`; each node's authored opinions visible in the exported layer stack.
  - size: M

**Verification gate:** CI green including M20.P3.T3; on real hardware, a scene built purely from create/edit nodes renders through `USDRender` to an image matching the committed reference, and the same scene round-trips through `USDWrite`/`USDRead`; gizmo manipulation works with undo; pre-existing ctest suite green.
