# M350 - Shader Manifest And Nodes

Generate one Natron shader node for each shader the bundled Cycles and MoonRay offer, with every attribute as a knob. Natron owns a JSON manifest generated from each renderer: Cycles' `NodeType` registry (sockets, types, defaults, enums, LINKABLE) and MoonRay's class JSON (name, type, default, group, label, comment, flags such as `FLAGS_BINDABLE`, including `lib/material/dwabase/json` includes). Sdr is skipped, because `moonray_sdr_plugins` uses the removed Ndr and is dropped in `build-moonray.sh`. Shader nodes output a new `eDataKindShader`, so they can only connect inside shader networks and are never rendered, viewed or cached. Applies M349 - Shader Node Spike's results on inputs, named outputs and registration; re-check this brief against its design note at claim.

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
- MaterialX nodes: none, set aside by the user.
- RenderMan or other renderers: none. The manifest format doesn't preclude them.

**Edge cases:**
- Saved shader or attribute no longer exists after a renderer upgrade: Natron's existing `MissingPluginNode` keeps the node's inputs, connections and values and saves them unchanged. A missing input gets the existing "left disconnected" load warning, and a missing knob gets a load warning naming it.
- New Cycles socket missing from the curated table: it lands in an "Other" group, and the drift check reports it.
- Array attributes (ramps, curves): map to Natron's parametric knob where the data is a curve. Any other array gets a text-editable table. Every array type is listed in the audit, none dropped.
- Shader node connected outside a network, or to a Viewer: refused by the data kind check.

**Assumptions:**
- Nothing authored by this milestone reaches a renderer yet. M353 - Cycles Native Shaders and M354 - MoonRay Native Shaders prove the attributes against the real delegates.

## Phase 350.1: Manifest

- [ ] M350.P1.T1 — Define the manifest and generate MoonRay's
  - files: `tools/shaders/generate_moonray_manifest.py`, `Engine/Nodes/Scene/Shaders/manifest/moonray.json`, `Engine/Nodes/docs/shader-manifest.md`
  - approach: define the schema: renderer, shader ID (`info:id`, the rdl2 class name), kind, attributes (name, type, default, enum values, linkable, group, comment), outputs. Generate from the MoonRay source's class JSON including its includes. Skip `dso/displayfilter`. The manifest is committed, so builds need no renderer.
  - verify: a Python check run from `Tests/` loads the output; `DwaBaseMaterial` has about 170 attributes with its groups; no display filter is present; regeneration is byte-stable.
  - size: M

- [ ] M350.P1.T2 — Generate Cycles' manifest
  - files: `tools/shaders/dump_cycles_manifest.cpp`, `tools/shaders/CMakeLists.txt`, `Engine/Nodes/Scene/Shaders/manifest/cycles.json`
  - approach: a small program linked against the bundled Cycles walks the `NodeType` registry, keeps shader nodes and drops OSL, and writes the same schema with `cycles_<type>` IDs as hdCycles expects (`src/hydra/material.cpp`). It records every output socket.
  - verify: run in the image; Principled BSDF has about 33 inputs; an image texture lists Color and Alpha outputs; no OSL node is present; regeneration is byte-stable.
  - size: M

- [ ] M350.P1.T3 — Curate Cycles groups and key ports
  - files: `Engine/Nodes/Scene/Shaders/manifest/cycles_layout.json`, `Engine/Nodes/Scene/Shaders/manifest/moonray_layout.json`, `tools/shaders/check_manifest.py`
  - approach: a hand-written group table for every Cycles shader, and a key-port list for each shader of either renderer with more than 12 ports. `check_manifest.py` fails on a stale name in either table, and reports attributes that fall into "Other".
  - verify: `check_manifest.py` passes; removing an attribute from a test copy of the manifest makes it fail; the Principled BSDF's groups cover all its attributes.
  - size: M

- [ ] M350.P1.T4 — Check manifests against the bundled renderers
  - files: `tools/shaders/check_manifest_drift.sh`, the image's renderer build scripts
  - approach: at image build, regenerate both manifests and compare them with the committed files, so a renderer upgrade can't change shaders silently.
  - verify: running the script against a deliberately edited manifest fails naming the attribute; against the committed one it passes.
  - size: S

## Phase 350.2: Nodes

- [ ] M350.P2.T1 — Add the Shader data kind
  - files: `Global/Enums.h`, `Engine/Node.cpp`, `Engine/Settings.cpp`, `Gui/Edge.cpp`, `Gui/TabMenuBridge.cpp`
  - approach: add `eDataKindShader`, following the `eDataKindRaw` precedent at each site (about 21 in `Node.cpp`). Connection checks accept shader to shader only. Output scheduling, viewers and caches never touch it. Tab menu kind filtering (`Gui/TabMenuBridge.cpp:127`) excludes it everywhere except shader graphs.
  - verify: gtest following `Tests/Native/RawKindFramework_Test.cpp` — shader output refused by image and scene inputs and by a Viewer; a shader node never reaches `render`; the existing kind tests pass.
  - size: M

- [ ] M350.P2.T2 — Build a shader node from the manifest
  - files: `Engine/Nodes/Scene/Shaders/ShaderNode.{h,cpp}`, `Engine/Nodes/Scene/Shaders/ShaderManifest.{h,cpp}`, `Tests/Scene/ShaderNode_Test.cpp`
  - approach: load the manifest once. For one entry, create knobs by type (part of M253 - Delegate Render Settings' `RenderSettingsKnobs` type mapping applies, plus enums, assets and parametric arrays), pages by group, and inputs for key ports, using M349 - Shader Node Spike's mechanism. Named outputs come from the same results. Hash covers knobs and connections only.
  - verify: gtest — Principled BSDF and `DwaBaseMaterial` have every manifest attribute as a knob with the right default and type; a save and load round trip restores edited values; animation and expressions work on a shader knob.
  - size: L

- [ ] M350.P2.T3 — Register every shader
  - files: `Engine/AppManager.cpp`, `Engine/Nodes/Scene/Shaders/ShaderNode.cpp`, `Tests/Scene/ShaderRegistration_Test.cpp`
  - approach: register per M349 - Shader Node Spike's choice, grouped per renderer and hidden from the Nodes menu, toolbar and main-graph Tab. Python's `createNode` works by ID. Startup stays lazy in the manifest load if the cost shows.
  - verify: gtest — every manifest entry creates a node by ID; none appears in the main-graph menu listing; startup time compared with `tools/bench` is unchanged within noise.
  - size: M

- [ ] M350.P2.T4 — Promote attributes to ports in the engine
  - files: `Engine/Nodes/Scene/Shaders/ShaderNode.{h,cpp}`, `Tests/Scene/ShaderPromote_Test.cpp`, Python bindings for the shader node
  - approach: promote and demote an attribute by script name, creating or removing its input. A connected port disables its knob. Demoting a connected port disconnects it. Promotions save with the node and are scriptable. Port-only attributes can't be demoted, and knob-only ones can't be promoted.
  - verify: gtest — promote, connect, save, load restores port and connection; demoting disconnects and re-enables the knob; a non-linkable attribute refuses promotion; undo restores the state.
  - size: M

- [ ] M350.P2.T5 — Handle missing shaders and audit coverage
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
