# M350 - Shader Manifest And Nodes

Generate one Natron shader node for each shader the bundled Cycles and MoonRay offer, with every attribute as a knob. Natron owns a JSON manifest generated from each renderer: Cycles' `NodeType` registry (sockets, types, defaults, enums, LINKABLE) and MoonRay's class JSON (name, type, default, group, label, comment, flags such as `FLAGS_BINDABLE`, including `lib/material/dwabase/json` includes). Sdr is skipped, because `moonray_sdr_plugins` uses the removed Ndr and is dropped in `build-moonray.sh`. Shader nodes are instances of the one generic network node (M349 - Shader Node Spike; `PLAN/DECISIONS/2026-11-17-generic-port-networks.md`), built from a spec. Every network node outputs the one new `eDataKindNetwork`, which keeps network nodes off comp inputs and out of rendering, viewing and caching; which nodes a container accepts is decided by their declared `targets`, and which ports connect by port types (`PortTypes`, M351 - Shader Graph Presentation). The manifest schema is the network node spec for every context: shader manifests are its first content, and M482 - Geometry VOP Node and F32 - Channel Networks add theirs. Applies M349 - Shader Node Spike's results on inputs, named outputs and registration; re-check this brief against its design note at claim. Ports are the single connection model (M349 - Shader Node Spike's 2026-11-22 decision): graph items sit on Node inputs plus a port descriptor, so this milestone carries each port's type, page and promoted state in the runtime spec, and M351 - Shader Graph Presentation draws from it. No new engine connection store.

## Scope

**In:**
- Every Cycles shader node except OSL, and every MoonRay material, map, normal map, displacement and volume shader. MoonRay's display filters aren't shaders and stay out.
- Every attribute is a knob with its renderer's default, type and enum values. Label is the readable script name (`base_color` → "Base Color"), and script name is the renderer's attribute name, sanitised.
- Linkable attributes can be promoted to ports. Closures and `SceneObject` bindings have no knob value and are always ports. Non-linkable attributes are knob-only.
- Panel groups: MoonRay's `group` metadata; a hand-curated, committed table for every Cycles shader.
- A key-port list for each shader with more than 12 ports. Smaller shaders show every linkable attribute as a port.
- Node docs generated from the manifest; tooltips use MoonRay's comments, and Cycles' socket name, type and default.

**Out:**
- Port drawing, promote control and tab menu: M351 - Shader Graph Presentation.
- Authoring USD: M352 - USDMaterialBuilder Node.
- OSL script nodes: none (decision `2026-11-05-no-cycles-osl`).
- MaterialX nodes: M446 - MaterialX Builder Nodes, which extends this manifest.
- RenderMan or other renderers: none. The manifest format doesn't preclude them.

**Edge cases:**
- Saved shader or attribute no longer exists after a renderer upgrade: Natron's existing `MissingPluginNode` keeps the node's inputs, connections and values and saves them unchanged. A missing input gets the existing "left disconnected" load warning, and a missing knob gets a load warning naming it.
- New Cycles socket missing from the curated table: it lands in an "Other" group, and the drift check reports it.
- Array attributes (ramps, curves): map to Natron's parametric knob where the data is a curve. Any other array gets a text-editable table. Every array type is listed in the audit, none dropped.
- Shader node connected outside a network, or to a Viewer: refused by the data kind check.

**Assumptions:**
- Nothing authored by this milestone reaches a renderer yet. M353 - Cycles Native Shaders and M354 - MoonRay Native Shaders prove the attributes against the real delegates.

## Phase 350.1: Manifest

- [x] M350.P1.T1 — Define the manifest and generate MoonRay's (06094ec78b)
  - files: `tools/shaders/generate_moonray_manifest.py`, `Engine/Nodes/Scene/Shaders/manifest/moonray.json`, `Engine/Nodes/docs/shader-manifest.md`
  - approach: define the schema: renderer, shader ID (`info:id`, the rdl2 class name), kind, attributes (name, type, default, enum values, linkable, group, comment), outputs, and `targets` (the targets a node supports, `cycles` or `moonray` here; generic kernel nodes later declare `kernel`). Nothing in the schema is shader-only: a kernel node's code template and a channel node's cook entry are later optional fields. Generate from the MoonRay source's class JSON including its includes. Skip `dso/displayfilter`. The manifest is committed, so builds need no renderer.
  - verify: a Python check run from `Tests/` loads the output; `DwaBaseMaterial` has about 170 attributes with its groups; no display filter is present; regeneration is byte-stable.
  - size: M

- [x] M350.P1.T2 — Generate Cycles' manifest (06094ec78b)
  - files: `tools/shaders/dump_cycles_manifest.cpp`, `tools/shaders/CMakeLists.txt`, `Engine/Nodes/Scene/Shaders/manifest/cycles.json`
  - approach: a small program linked against the bundled Cycles walks the `NodeType` registry, keeps shader nodes and drops OSL, and writes the same schema with `cycles_<type>` IDs as hdCycles expects (`src/hydra/material.cpp`). It records every output socket.
  - verify: run in the image; Principled BSDF has about 33 inputs; an image texture lists Color and Alpha outputs; no OSL node is present; regeneration is byte-stable.
  - size: M

- [x] M350.P1.T3 — Curate Cycles groups and key ports (06094ec78b)
  - files: `Engine/Nodes/Scene/Shaders/manifest/cycles_layout.json`, `Engine/Nodes/Scene/Shaders/manifest/moonray_layout.json`, `tools/shaders/check_manifest.py`
  - approach: a hand-written group table for every Cycles shader, and a key-port list for each shader of either renderer with more than 12 ports. `check_manifest.py` fails on a stale name in either table, and reports attributes that fall into "Other".
  - verify: `check_manifest.py` passes; removing an attribute from a test copy of the manifest makes it fail; the Principled BSDF's groups cover all its attributes.
  - size: M

- [x] M350.P1.T4 — Check manifests against the bundled renderers
  - files: `tools/shaders/check_manifest_drift.sh`, the image's renderer build scripts
  - approach: at image build, regenerate both manifests and compare them with the committed files, so a renderer upgrade can't change shaders silently.
  - verify: running the script against a deliberately edited manifest fails naming the attribute; against the committed one it passes.
  - size: S

## Phase 350.2: Nodes

- [x] M350.P2.T1 — Add the Shader data kind
  - files: `Global/Enums.h`, `Engine/Node.cpp`, `Engine/Settings.cpp`, `Gui/Edge.cpp`, `Gui/TabMenuBridge.cpp`
  - approach: add `eDataKindNetwork`, following the `eDataKindRaw` precedent at each site (about 21 in `Node.cpp`). Connection checks accept network to network only; which network ports may join is left to `PortTypes`. Output scheduling, viewers and caches never touch it. Tab menu kind filtering (`Gui/TabMenuBridge.cpp:127`) excludes it everywhere except shader graphs.
  - verify: gtest following `Tests/Native/RawKindFramework_Test.cpp` — shader output refused by image and scene inputs and by a Viewer; a shader node never reaches `render`; the existing kind tests pass.
  - size: M

- [x] M350.P2.T6 — Carry a port descriptor in the runtime spec
  - files: `Engine/Nodes/NodeSpec.h`, `Engine/Nodes/Network/PortDescriptor.{h,cpp}`, `Engine/Node.{h,cpp}`, `Engine/Nodes/Network/NetworkNode.{h,cpp}`, `Tests/Network/PortDescriptor_Test.cpp`
  - approach: add a runtime `PortDescriptor` (attribute name, renderer type name, core type class, page, promoted, array, varying) for each input, and an output descriptor list (name, type, type class, array) for each node. `InputSpec` is static (`std::string_view` fields, `NodeSpec.h:88-97`), so runtime-built network nodes hold their descriptors on the node and `Node` exposes `getInputPortDescriptor(i)` and `getOutputPorts()`; classic nodes return none. The core type classes are float, int, vector3, normal, point, color, string, closure, displacement, texture, channel and custom; each target maps its renderer's types onto them in one table. Promoted is live state (M350.P2.T4 flips it), and saves with the node. The downstream node stores the chosen output name per input, as `PortTestNode` does (`Engine/Nodes/Test/PortTestNode.h:118-131,187-188`). Nothing here draws.
  - verify: gtest — a network node built from a two-input, two-output test spec reports each descriptor's type class, page, array and varying flags; a classic node reports none; promoted state and a connected output name survive a save and load.
  - size: M

- [x] M350.P2.T2 — Build a shader node from the manifest
  - files: `Engine/Nodes/Network/NetworkNode.{h,cpp}`, `Engine/Nodes/Scene/Shaders/ShaderManifest.{h,cpp}`, `Tests/Scene/ShaderNode_Test.cpp`
  - approach: load the manifest once into network node specs. `NetworkNode` is the one generic class: nothing in it names shaders or renderers. For one entry, create knobs by type (part of M253 - Delegate Render Settings' `RenderSettingsKnobs` type mapping applies, plus enums, assets and parametric arrays), pages by group, and inputs for key ports, each with its port descriptor (M350.P2.T6), using M349 - Shader Node Spike's mechanism. Named outputs come from the same results. Hash covers knobs and connections only.
  - verify: gtest — Principled BSDF and `DwaBaseMaterial` have every manifest attribute as a knob with the right default and type; a save and load round trip restores edited values; animation and expressions work on a shader knob.
  - size: L

- [ ] M350.P2.T3 — Register every shader
  - files: `Engine/AppManager.cpp`, `Engine/Nodes/Network/NetworkNode.cpp`, `Tests/Scene/ShaderRegistration_Test.cpp`
  - approach: register per M349 - Shader Node Spike's choice, grouped per renderer and hidden from the Nodes menu, toolbar and main-graph Tab. Python's `createNode` works by ID. Startup stays lazy in the manifest load if the cost shows.
  - verify: gtest — every manifest entry creates a node by ID; none appears in the main-graph menu listing; startup time compared with `tools/bench` is unchanged within noise.
  - size: M

- [x] M350.P2.T4 — Promote attributes to ports in the engine
  - files: `Engine/Nodes/Network/NetworkNode.{h,cpp}`, `Tests/Scene/ShaderPromote_Test.cpp`, Python bindings for the shader node
  - approach: promote and demote an attribute by script name, creating or removing its input. A connected port disables its knob. Demoting a connected port disconnects it. Promotions save with the node and are scriptable. Port-only attributes can't be demoted, and knob-only ones can't be promoted.
  - verify: gtest — promote, connect, save, load restores port and connection; demoting disconnects and re-enables the knob; a non-linkable attribute refuses promotion; undo restores the state.
  - size: M

- [x] M350.P2.T5 — Handle missing shaders and audit coverage
  - files: `Tests/Serialization/ShaderMissing_Test.cpp`, `tools/shaders/audit_shader_nodes.py`, generated docs under `Engine/Nodes/Scene/docs/shaders/`
  - approach: confirm a removed shader loads as `MissingPluginNode`, keeping inputs, values and connections, and saves unchanged. Audit each manifest entry against its created node's knobs, ports and defaults. Generate per-shader docs.
  - verify: gtest — a project with an unknown shader ID round-trips byte-equivalent in its node's stored data, with its connections intact; the audit reports zero missing attributes across both renderers.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `Shader*_Test` and `check_manifest.py` pass; the audit reports zero missing attributes across every shader of both renderers.

## Decisions

- 2026-11-05 — **Hand-curated Cycles groups (user):** chosen over name-prefix grouping and a flat list, accepting that each Cycles upgrade needs review. The drift check makes the review unavoidable.
- 2026-11-05 — **Missing shaders use Natron's normal path, without losing inputs (user):** the existing `MissingPluginNode` already keeps inputs and values (`Engine/NodeGroupSerialization.cpp:300-312`), so no new placeholder.
- 2026-11-05 — **Knob-first attributes (user):** every value attribute is a knob; ports only for key and promoted ones.
- 2026-11-05 — **Key-port lists for shaders over 12 ports (user chose key ports; threshold cat-plan):** small shaders show all of theirs.
- 2026-11-05 — **Committed manifest with a drift check (cat-plan):** builds and GUI tests need no renderer, and diffs are reviewable at each upgrade.
- 2026-11-17 — **One network data kind; targets, then port types (user):** `eDataKindNetwork` replaces `eDataKindShader`; contexts add no new kinds. Generic ops such as Add are one node usable in any container whose target it supports.
- 2026-11-17 — **Houdini's target model, no lowering (user):** nodes declare supported targets; renderer builders show only renderer-native nodes (Cycles Math, not a generic Add), and nothing translates generic nodes into renderer nodes.
- 2026-11-22 — **Port descriptor in the runtime spec (user, via M349 - Shader Node Spike):** ports are the single connection model; each input carries type, page and promoted state, and each output its name and type, so the graph needs no connection store of its own. Added as M350.P2.T6 (cat-plan).
- 2026-11-25 (PM) — P1.T1-T3 in 06094ec78b, generated in the published image fea31bf10eb9466e (Cycles 18a83a2, openmoonray ee4955e).
  - **MoonRay:** 112 shaders (125 classes less 13 Test* classes) and 2628 attributes. DwaBaseMaterial has 106 attributes in 12 groups. The "about 170" in T1's verify was wrong; M349 found about 101.
  - **Cycles:** 94 shaders and 575 attributes. The dump tool loads hdCycles.so's node registry and compiles against the same Cycles source.
  - **Checks:** `check_manifest.py` passes and `ShaderManifestPython` is registered.
  - **Decisions:**
    - MoonRay's Test* classes are dropped from the manifest, since they aren't user shaders.
    - The Cycles kinds stay shader/texture/displacement/utility.
    - T4's drift check runs at image build: build-cycles.sh keeps the Cycles source the dump tool needs, and dev-image.yml runs check_manifest_drift.sh in the built image before pushing (option a).
- 2026-11-25 (PM) — Phase 350.2 and P1.T4 in ee621bda9f + 0391193ff3; m350-gate2 green (run 45729402808). F2's branch had only M349 on an old main, so main (83241b6529) was merged into it (d6ff04dfde). PR #386 into F2 is in review. Still owed: P2.T3's startup-time comparison (needs an AppImage against a main AppImage), and the drift check's first real run when dev-image.yml next builds the image.
- 2026-11-25 (PM) — PR #386 review round 1 (18 findings, posted on the PR) addressed in the working tree: ports always saved, linked knobs stay disabled, upstream map filled after load, `canConnectInput` refuses through `checkConnection`, Python port edits check editability and are undoable, sanitized ports written back, retired specs bounded, curve defaults from the manifest, MoonRay ramps as curve knobs, readable MoonRay labels, lazy layout building, drift check reruns `check_manifest.py`, and tests for each. A Viewer wired to a shader shows nothing without errors. The data-kind sweep found the other sites already safe. `mix` on the seven ungrouped ColorCorrect*Map nodes moves to the end of the main page with its name and value unchanged; kept.
