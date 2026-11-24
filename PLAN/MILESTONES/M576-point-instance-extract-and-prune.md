# M576 - Point Instance Extract And Prune

Let a user turn chosen instances of a PointInstancer into ordinary prims they can edit, and remove single instances without removing the whole instancer. Today no Scene node addresses one instance. USDPrune (`Engine/Nodes/Scene/USDPrune.cpp`, `renderScene`) matches whole prims through `SceneNodeAuthoring::matchPattern` and authors `SetActive(false)` or `visibility = invisible` on each target. Its Prim Pattern is a `PrimSelector` USD path expression (`SceneOps::parsePrimExpression`), which has no instance syntax. M423 - USDPrune Isolate Mode recorded "pruning point instances one by one: no milestone". The only other PointInstancer code is the prototype lookup in `USDAssignMaterial.cpp` (`getPointInstancerPrototypes`) and the instancer mask in `SceneDrawFilter::withMask`, which already honours an authored `invisibleIds` mask in the viewer. This milestone adds a new node, USDExtractInstances (modelled on Solaris Extract Instances), and `/path[index]` entries to USDPrune's Prim Pattern. Both share one instance-selection parser. Instancers come from any source, such as USDRead or F15's GeoToUSD "Point Instancer" packed mode (`Engine/Nodes/Geometry/GeoToUSD.cpp`, on the unmerged F15 branch). Tests author their own small instancers.

## Scope

**In:**
- A shared instance selection, `InstanceSelector`: space-separated entries `/path[i]`, `/path[i j]`, `/path[a-b]` (inclusive) and `/path[*]`. The path is a literal absolute prim path. An index is the instance's position in `protoIndices`, as in Solaris. When the instancer authors `ids`, the index is translated to the id the node writes into `invisibleIds` and `inactiveIds`.
- USDPrune with instance entries: plain entries keep going through `PrimSelector`, and `[...]` entries are split off before that.
  - Deactivate appends the ids to the instancer's `inactiveIds` (an `SdfInt64ListOp` with appended items, so it composes over the input's).
  - Hide authors `invisibleIds` as the union of the input's value and the selected ids. When the input's `invisibleIds` or `ids` vary over time, it authors one sample per frame of `getSampleFrames()`.
  - Prune Unselected with instance entries keeps the instancer and its prototypes, and prunes every other instance of that instancer by Mode. The rest of the scene is isolated as M423 does today.
- New node USDExtractInstances, registered in the `PLUGIN_GROUP_USD_*` Edit submenu (M520 - USD Toolbar Category And Submenus). It has one Scene input and authors only in its own layer. The Solaris Extract Instances controls, each In or Out:
  - **Type** (`type`: Point Instancer / Native Instances): In.
  - **Instances** (`instances`, an InstanceSelector for Point Instancer, a PrimSelector pattern for Native Instances): In. The VEXpression form `[{@attr>…}]` is Out.
  - **Primitive Path** (`primPath`, default `/extracted`): In. Each extracted instance is created at `<primPath>/<instancerName>/instance_<index>`, under an `Xform` named after the instancer.
  - **Primitive Name** (`primName`, empty by default): In. When it is not empty, it replaces the `instance` prefix. Solaris's enable toggle plus text field becomes one string, where empty means off.
  - **Flatten Hierarchy** (`flattenHierarchy`, off): In. Extracted prims go directly under Primitive Path, and a path clash is a node error.
  - **Prototype** (`prototype`: From Instances / From Existing Primitives): In.
  - **Prototype Primitive** (`prototypePrim`): In. It is enabled with From Existing Primitives.
  - **Modify Source Primitive** (`modifySource`: Do Not Modify / Hide Source Primitive): In. It is enabled with From Existing Primitives.
  - **Make Instanceable** (`makeInstanceable`, off): In. It sets `instanceable = true` on the extracted reference prims.
  - **Bind Materials** (`bindMaterials`, off): In. It binds the material that resolves for the instancer to each extracted prim.
  - **Deactivate Entirely Extracted Instancers** (`deactivateExtractedInstancers`, off): In.
  - **Scalar Primvars With Unauthored Element Size** (`scalarPrimvars`, on): In.
  - **Source Mode** (`sourceMode`: Hide (default) / Deactivate, non-animatable): added, as USDPrune's Mode does for prims (user, 2026-11-24). Solaris only hides. Hide authors the source instance into `invisibleIds`, and Deactivate into `inactiveIds`; for native instances, Hide authors `visibility = invisible` and Deactivate authors `active = false`.
  - **Extraction Frame: Method** (Frame / Time): Out. Natron knobs are frame-based, so Time is redundant.
  - **Frame** plus Solaris's `$F` default: In as **Freeze Extraction** (`freezeExtraction`, off) and **Frame** (`frame`, enabled when Freeze Extraction is on). Off follows the input's animation. On extracts once at Frame and authors no time samples.
  - **Integer Frames**: Out. The node samples only at whole frames (`getSampleFrames()`), so it never cooks a fractional frame.
- How extraction works:
  - Each extracted prim gets an internal reference to its prototype (the `prototypes` target for `protoIndices[i]`, or Prototype Primitive).
  - Each gets one `xformOp:transform` matrix with its own `xformOpOrder`: the instance's world transform (`ComputeInstanceTransformsAtTime` with the prototype's own transform included, times the instancer's local-to-world), made relative to the new parent.
  - The source instance is removed by Source Mode: Hide authors an `invisibleIds` union, as in USDPrune Hide; Deactivate appends to `inactiveIds`, as in USDPrune Deactivate.
  - Per-instance primvars on the instancer (vertex or varying interpolation) become constant primvars holding that instance's element or elements. Constant primvars on the instancer are copied as they are.
- Time-varying inputs: when the instancer's `positions`, `orientations`, `scales`, `protoIndices`, `ids` or any ancestor transform vary over time, the transform (and the visibility of an instance that comes and goes) is authored at every frame of `getSampleFrames()`.
- Native Instances type: each matched instanceable prim gets a non-instanceable copy (an internal reference with `instanceable = false`) at `<primPath>/<name>`, with the matched prim's world transform. The source prim is hidden or deactivated by Source Mode.
- Node docs `Engine/Nodes/Scene/docs/USDExtractInstances.md`, an updated `docs/USDPrune.md`, gtests and an Xvfb GUI script.

**Out:**
- Editing, retiming or reassigning prototypes of instances (Solaris Modify Point Instances, Retime Instances, Assign Prototypes): not chosen (user, 2026-11-24). No milestone.
- Nested instance selection `/path[2][7]`: none. After extracting the outer instance, the inner instancer is a real prim under the extracted prim, so a second USDExtractInstances or USDPrune reaches it by plain path.
- VEXpression instance selection (`/path[{@weights>0.5}]`) and wildcards in the path of a `[...]` entry: none. There is no attribute-expression language for scene patterns.
- Picking single instances in the viewer or in the scene graph panel to fill these knobs: no milestone. Paths and indices are typed in.
- The rest of Solaris Prune's controls (bounding-box, type, kind, purpose and VEXpression target methods, exclusion rules, Prune Restrictions, Create Missing Explicit Prims As Overs, the animated Prune/un-prune value, Prune Ancestors Automatically, Prune Class Primitives, Create Collection): no milestone, as M423 records. They are not instance handling.
- Solaris Prune's **Prune Point Instances Individually**: Out. It only matters for bounding-box and expression targets, which USDPrune lacks. Explicit `[...]` entries prune single instances, and a plain instancer path still prunes the whole instancer, as Solaris does with the toggle off.
- Sub-frame motion of extracted prims (instancer `velocities`, `accelerations`, `angularVelocities`): none. Extracted prims get whole-frame transform samples and USD interpolates between them.

**Edge cases:**
- An index past the instance count at the evaluated frame: skipped with a node warning naming it. If no entry resolves, the input passes through unchanged with a warning, like USDPrune's no-match case.
- An instancer whose instance count changes over time (births and deaths in a sim): the extracted prim's `visibility` is sampled per frame, invisible where its index does not exist.
- `protoIndices[i]` changes over time: the prototype at the first sampled frame (or at Frame when frozen) is used, with a warning that the instance switches prototype.
- A path in an instance entry that is not a PointInstancer: node error "<path> is not a point instancer".
- An instancer, or Prototype Primitive, inside an unloaded payload in the viewer: not seen (no payload loading without user action). This is reported as no match. Renders load everything.
- An instancer inside a native instance: USD cannot author inside an instance, so this is a node error naming the enclosing instance.
- Primitive Path inside the instancer, or inside an instance: node error.
- Primitive Path already exists on the input: extracted prims go under it, relative to its transform.
- Deactivate Entirely Extracted Instancers when only some instances are extracted: the instancer stays active.
- USDPrune Deactivate and Hide on the same instance across two nodes: `inactiveIds` and `invisibleIds` compose independently, and the instance stays gone.
- Extracting an instance the input already deactivated through `inactiveIds`: skipped with a warning, since there is nothing to show.

**Assumptions:**
- UsdImaging folds both `inactiveIds` and `invisibleIds` into the instancer mask that Storm and the other delegates honour, and `SceneDrawFilter::withMask` keeps an authored mask (its comment says it does).
- An internal reference to a prototype under the instancer composes correctly when the extracted prim sits outside the instancer, and material bindings inside the prototype are remapped by the reference.
- A stronger layer's `invisibleIds` default overrides weaker time samples, so the union has to be authored per frame whenever the input's value varies.

## Phase 576.1: Instance selection

- [ ] M576.P1.T1 — Parse and resolve instance entries
  - files: `Engine/Nodes/Scene/InstanceSelector.{h,cpp}` (new), `Tests/Scene/InstanceSelector_Test.cpp` (new), the Scene test list in `Tests/CMakeLists.txt` if tests are listed there
  - approach: a USD-free parser, `InstanceSelector::split(text, &plainExpression, &entries, &error)`. It pulls out `<abs path>[list]` tokens, where list is space-separated indices, `a-b` ranges or `*`, and leaves the rest as a PrimSelector expression. Reject a wildcard path or a nested `[..][..]`. Add a USD-side `resolve(stage, entry, time, &result)` that checks the prim is a `UsdGeomPointInstancer`, reads `protoIndices` and `ids` at time, and returns the valid indices, the id for each one (the index when `ids` is unauthored) and the indices that are out of range.
  - verify: gtest — parsing of single indices, lists, ranges, `*`, mixed plain and instance text, and the rejected forms with their messages. Resolution on an in-memory stage with an instancer of 5 instances, with and without authored `ids`, a range running past the end, and a Mesh path (error).
  - size: M

## Phase 576.2: USDPrune instances

- [ ] M576.P2.T1 — Prune single instances by Mode
  - files: `Engine/Nodes/Scene/USDPrune.{h,cpp}`, `Tests/Scene/USDPruneInstances_Test.cpp` (new)
  - approach: in `renderScene`, run `InstanceSelector::split` on Prim Pattern before `matchPattern`. Pass the plain part on as today, and resolve the instance entries on the leased upstream stage. Add each instancer path to `args.context.reach`. In the author func, Deactivate authors `inactiveIds` as an `SdfInt64ListOp` with appended items. Hide authors `invisibleIds` as the union with the input's value: a default when the input's `invisibleIds` and `ids` are not time-varying, else one sample per `getSampleFrames()` frame. Add the instancer paths, ids and sample frames to the `facts` digest. Keep the no-match warning and the error for a bad entry.
  - verify: gtest — `/inst[1 3]` in Deactivate leaves `ComputeMaskAtTime` false at 1 and 3. Hide over an input that already hides 0 leaves 0, 1 and 3 hidden. An animated input `invisibleIds` is unioned per frame. `ids` = {10..14} with `[1]` authors 11. A mixed pattern `/World/Box /inst[2]` prunes both. The existing `USDMergePrune_Test` still passes.
  - size: L

- [ ] M576.P2.T2 — Isolate around instance entries
  - files: `Engine/Nodes/Scene/USDPrune.cpp`, `Tests/Scene/USDPruneInstances_Test.cpp`
  - approach: with Prune Unselected on, treat each instance entry's instancer and its `prototypes` targets as kept paths for `findIsolateRoots`. Prune that instancer's unselected instances by Mode, using the P2.T1 authoring path. Include the instance entries in the `IsolateCache` key.
  - verify: gtest — isolating `/inst[2]` keeps the instancer and its prototypes, leaves only instance 2 unmasked, and prunes an unrelated Mesh. Keep Lights still keeps a light. A cached second render gives the same layer.
  - size: M

- [ ] M576.P2.T3 — Document instance entries in USDPrune
  - files: `Engine/Nodes/Scene/docs/USDPrune.md`, `Engine/Nodes/Scene/USDPrune.{h,cpp}` (`kNodeSpec.description`, Prim Pattern purpose text, the Mode and Prune Unselected tooltips)
  - approach: describe the `[...]` syntax, what an index is and how it maps to ids, Deactivate versus Hide for instances, and isolating around an instance. Keep the existing style, and name no other application.
  - verify: the docs and tooltips describe what P2.T1 and P2.T2 implement, and a grep for Houdini, Solaris, Nuke, Katana and Gaffer in the changed files is empty.
  - size: M

## Phase 576.3: USDExtractInstances node

- [ ] M576.P3.T1 — Add the node and its knobs
  - files: `Engine/Nodes/Scene/USDExtractInstances.{h,cpp,reg.inc}` (new), its node icon beside the other Scene node icons, `Tests/Scene/ScenePrimPathKnobSpec_Test.cpp`
  - approach: mirror USDPrune's structure (NodeSpec with one Scene input, `.grouping` set to the M520 Edit submenu, `primPathKnob = instances`). Create every In knob from ## Scope with its script name, readable label, default and tooltip, all non-animatable except `frame`. Make the enable states follow Type, Prototype and Freeze Extraction (`refresh…Knobs` from `knobChanged` and `onKnobsLoaded`). `renderScene` passes the input through for now.
  - verify: gtest — the node creates, each knob exists with its default, and the enable states follow Type, Prototype and Freeze Extraction. The prim-path knob spec test lists the node.
  - size: M

- [ ] M576.P3.T2 — Extract point instances from their prototypes
  - files: `Engine/Nodes/Scene/USDExtractInstances.cpp`, `Tests/Scene/USDExtractInstances_Test.cpp` (new)
  - approach: resolve Instances with `InstanceSelector`. For each instance, define the parent `Xform` and the extracted prim at the path from Primitive Path, Primitive Name and Flatten Hierarchy (a clash is an error). Add an internal reference to its prototype. Author `xformOp:transform` from the instance's world transform made relative to the new parent's (upstream's world transform when Primitive Path exists). Remove the source instance by Source Mode, reusing the USDPrune P2.T1 authoring: Hide authors the `invisibleIds` union, and Deactivate appends to `inactiveIds`. Emit with `eUpstreamReadFacts` over the instancer paths, the resolved ids and the transforms read. Static inputs only in this task: author defaults at the current frame.
  - verify: gtest — 3 extracted instances of a static instancer with a translated and rotated parent and a prototype with its own transform. Each extracted prim's world bound matches the instance's (`ComputeExtentAtTime` per instance), the sources are masked through `invisibleIds` with Source Mode set to Hide and through `inactiveIds` with Deactivate (the input's `invisibleIds` unchanged), the extracted prims are editable (not instance proxies), and the Flatten clash and the bad-path errors fire.
  - size: L

- [ ] M576.P3.T3 — Follow or freeze the input's animation
  - files: `Engine/Nodes/Scene/USDExtractInstances.cpp`, `Tests/Scene/USDExtractInstances_Test.cpp`
  - approach: detect time variation on `positions`, `orientations`, `scales`, `protoIndices`, `ids` and the instancer's ancestor transforms (`ValueMightBeTimeVarying`, `TransformMightBeTimeVarying`). When they vary and Freeze Extraction is off, author the transform (and, with Source Mode set to Hide, the `invisibleIds` union) at every `getSampleFrames()` frame. Author `visibility` samples for an index that is missing at some frames, and warn when `protoIndices[i]` changes. When Freeze Extraction is on, evaluate once at Frame and author defaults. Add the sample frames and the frozen frame to the facts.
  - verify: gtest — an instancer with animated positions gives extracted prims whose world positions match per frame. An instance missing at frame 3 is invisible there. Freeze at frame 2 authors no time samples, and the values match frame 2.
  - size: M

- [ ] M576.P3.T4 — Carry primvars and materials, and finish extracted instancers
  - files: `Engine/Nodes/Scene/USDExtractInstances.cpp`, `Tests/Scene/USDExtractInstances_Test.cpp`
  - approach: copy the instancer's vertex and varying primvars as constant primvars holding element i (an `elementSize` slice). When Scalar Primvars With Unauthored Element Size is on and the size is unauthored, author a scalar. Copy constant primvars as they are. Bind Materials binds the instancer's computed bound material (`UsdShadeMaterialBindingAPI::ComputeBoundMaterial`) to each extracted prim. Make Instanceable sets `instanceable`. Deactivate Entirely Extracted Instancers deactivates an instancer when every instance was extracted.
  - verify: gtest — a per-instance `displayColor` gives each extracted prim its own colour (scalar versus array by the toggle). A material bound on the instancer resolves on the extracted prims when Bind Materials is on. Extracting `[*]` deactivates the instancer only when the toggle is on.
  - size: M

- [ ] M576.P3.T5 — Extract from an existing prim
  - files: `Engine/Nodes/Scene/USDExtractInstances.cpp`, `Tests/Scene/USDExtractInstances_Test.cpp`
  - approach: with Prototype set to From Existing Primitives, reference Prototype Primitive instead of the instance's prototype, keeping the instance's transform. Modify Source Primitive set to Hide Source Primitive authors `visibility = invisible` on Prototype Primitive. A missing or unloaded Prototype Primitive is an error.
  - verify: gtest — a hero Sphere replaces instance 1 at its transform. The source Sphere is hidden only with Hide Source Primitive. A missing path errors.
  - size: M

- [ ] M576.P3.T6 — Extract native instances
  - files: `Engine/Nodes/Scene/USDExtractInstances.cpp`, `Tests/Scene/USDExtractInstances_Test.cpp`
  - approach: with Type set to Native Instances, match Instances through `PrimSelector`, keeping only instanceable prims. For each, define `<primPath>/<name>` with an internal reference to it, `instanceable = false`, and its world transform relative to the new parent (sampled as in P3.T3). Hide or deactivate the source by Source Mode. Primitive Name, Flatten Hierarchy, Prototype, Bind Materials and Make Instanceable apply as for point instances.
  - verify: gtest — two instanceable references give two editable copies at the same world bounds, the sources are invisible with Hide and inactive with Deactivate, and a non-instanceable match is skipped with a warning.
  - size: M

- [ ] M576.P3.T7 — Write the node docs and audit the controls
  - files: `Engine/Nodes/Scene/docs/USDExtractInstances.md` (new), `Engine/Nodes/Scene/USDExtractInstances.{h,cpp}` (description and tooltips)
  - approach: write the node doc in the style of `docs/USDPrune.md`: the selection syntax, output paths, prototypes, time behaviour, the payload rule and the error and warning cases from ## Scope. Then do a params→knob audit: go through every control in `.local/docs/houdini/nodes/lop/extractinstances.md` and confirm its knob exists with the planned label and default, or that it is listed Out. Put the audit table in the PR body only, not in the repo.
  - verify: every Solaris control is accounted for in the audit, labels match their script names, and a grep for other application names in the docs and the source is empty.
  - size: M

- [ ] M576.P3.T8 — GUI check for both nodes
  - files: `Tests/gui/usd_extract_instances.py` (new), a small `.usda` fixture with one PointInstancer under `Tests/fixtures/` (text, a few KB)
  - approach: under Xvfb at 1920x1080, build USDRead (fixture) → USDExtractInstances (`/World/inst[0 2]`) → USDPrune (`/World/inst[4]`, Hide) → viewer. Assert that the extracted prims appear in the scene graph panel, that the instancer mask hides 0, 2 and 4, and that editing Primitive Path moves the extracted prims with no stale prims. Take screenshots of the viewer and of the USDExtractInstances panel.
  - verify: the script passes under Xvfb, and the screenshots go to the user one file each.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `InstanceSelector_Test`, `USDPruneInstances_Test`, `USDExtractInstances_Test`, `USDMergePrune_Test` and `ScenePrimPathKnobSpec_Test` pass; `Tests/gui/usd_extract_instances.py` and `Tests/gui/scene_prune_isolate.py` pass under Xvfb at 1920x1080; the user OKs the viewer and panel screenshots.

## Decisions

- 2026-11-24 — **Extract and prune only (user):** this milestone hero-extracts PointInstancer instances into editable prims and prunes instances by id. Editing, retiming and assigning prototypes are out.
- 2026-11-24 — **Pick by index (user):** instance entries use the instance index, as Solaris does, and are translated to `ids` when the instancer authors them.
- 2026-11-24 — **Native instances in (user):** USDExtractInstances keeps the Native Instances Type.
- 2026-11-24 — **Source Mode knob (user):** how the extracted source is removed is a knob, like USDPrune's Mode: Hide (`invisibleIds`, the default) or Deactivate (`inactiveIds`).
- 2026-11-24 — USDPrune's Deactivate maps to `inactiveIds` and Hide to `invisibleIds` for instance entries, mirroring its prim modes (planner).
