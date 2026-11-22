# M349 - Shader Node Spike

Prove that real Natron nodes are affordable and workable as shader nodes before any shader GUI is built, as the first member of F2 - Native Shader Networks. A network is a Group of shader nodes (design decision `PLAN/DECISIONS/2026-11-05-native-shader-networks.md`). Each shader carries about 170 knobs at the top end (MoonRay's `DwaBaseMaterial`), but only its key and promoted attributes are ports, so only those need to be Natron inputs. Three things are unproven: the knob cost at that scale, changing a node's inputs while it is live, and connections that name one of several outputs. Natron stores a connection as input label to node name (`connectSerializedInputs`, `Engine/NodeGroupSerialization.cpp:63`), so a node has one output today, while Cycles shaders have several. The network core is generic (user, 2026-11-17; `PLAN/DECISIONS/2026-11-17-generic-port-networks.md`): one network node class and one container class for every context, typed ports, and each node declaring the targets it supports, as Houdini's render mask does. Wiring checks the node's targets first, then port types. Shader targets come first; M482 - Geometry VOP Node and F32 - Channel Networks reuse the core unchanged, so the spike measures and prototypes that shape rather than a shader-only one.

## Scope

**In:**
- Measurements against the fixed bar: a 50-node network, using real attribute sets, opens in under 2 s and saves in under 2 s; a knob edit re-authors the layer in under 100 ms; comp graph edits are no slower with the network loaded.
- Live inputs: add and remove inputs on an existing node, with connections, undo and a save and load round trip.
- A connection naming which output it uses, saved and restored.
- A throwaway drawing prototype, settling whether `Edge` can draw port to port or needs a sibling class.
- Whether shaders register as one plug-in each or as one class with a runtime spec.
- A port-type and target model the note specifies for every planned context: shader targets (Cycles, MoonRay, later MaterialX), the kernel target (geometry, particle, image and channel VOPs) and channel sets (F32 - Channel Networks). The note sketches one example network per context to show nothing shader-specific leaks into the core.
- A design note carrying all of this, landed with the bench and tests.

**Out:**
- Production shader nodes and the manifest: M350 - Shader Manifest And Nodes.
- Production port drawing: M351 - Shader Graph Presentation. The prototype doesn't land.
- Delegate rendering: M353 - Cycles Native Shaders, M354 - MoonRay Native Shaders.

**Edge cases:**
- A bar is missed: report the numbers and the options, including lazy knob creation and the data-model design. Set `Needs: response`. Only the user may leave the real-node design.
- Live input changes turn out to be unsafe: measure the design where every attribute is a real input and report that too, since the user may still accept it.

**Assumptions:**
- `MissingPluginNode`'s runtime `NodeSpec` (`Engine/Nodes/MissingPluginNode.h:72`) is a usable starting point. It is set at load, so live change is unproven.
- Timings are from the dev box. They decide this design and are not a recurring CI gate.

## Phase 349.1: Prove it

- [x] M349.P1.T1 — Measure knob and network cost
  - files: `Tests/Scene/ShaderNodeCost_Test.cpp`, `tools/bench/shader_node_bench.py`, `Engine/Nodes/docs/shader-nodes.md`
  - approach: build a test-only effect whose knobs copy the attribute types and counts of `DwaBaseMaterial` (about 170), Cycles Principled BSDF (about 33) and a median MoonRay map (about 4), each with about 10 inputs. Build a 50-node mixed network in a Group. Time creation, open, save, knob edit plus re-authoring a layer through `SceneNodeAuthoring::openLayerStage`, and comp edits through `tools/bench/graph_bench.py` with the Group present. Repeat with every attribute as a real input, for comparison only.
  - verify: the bench prints each bar with pass or fail, the gtest builds and round-trips the network, and the numbers are in the design note.
  - size: L

- [x] M349.P1.T2 — Prove live inputs on an existing node
  - files: `Tests/Scene/ShaderNodeInputs_Test.cpp`, `Engine/Nodes/docs/shader-nodes.md`, test-only effect beside the test
  - approach: starting from `MissingPluginNode`'s runtime spec and Group input handling, add and remove named inputs on a live node. Exercise connected and unconnected inputs, undo, cycle checks and `getInputNumberFromLabel`. Also compare one plug-in per shader with one class and a runtime spec, by registration cost and by tab menu and Python consequences. The class must be the single network node class every context uses (shaders, generic kernel nodes, channel nodes), so a runtime spec is the expected outcome unless the numbers rule it out; a miss goes to the user.
  - verify: gtest — adding an input keeps existing connections; a save and load round trip restores promoted inputs and their connections; removing a connected input disconnects it cleanly. The note names the registration choice.
  - size: L

- [x] M349.P1.T3 — Prove connections that name an output
  - files: `Tests/Scene/ShaderNodeOutputs_Test.cpp`, `Engine/Nodes/docs/shader-nodes.md`, test-only effect beside the test
  - approach: compare storing the output name per input in a hidden knob on the downstream node with extending the serialized `inputs` map. Pick the smaller change that survives copy and paste, Group export and Python.
  - verify: gtest — an input connected to a named output of a two-output node keeps that name through save, load, and copy and paste. The note records the choice.
  - size: M

- [ ] M349.P1.T4 — Prototype port drawing and report
  - files: scratch `NodeGui` subclass and edge (not landed), `Engine/Nodes/docs/shader-nodes.md`
  - approach: dispatch a prototype generic port node (`PortNodeGui`, nothing shader-specific in it) as a `NodeGui` subclass in `NodeGraph.cpp:468` beside `DotGui` and `BackdropGui`. Draw left-to-right named ports and one port-to-port edge. Decide whether `Edge` (`Gui/Edge.h`, tied to an input number and an angle around the node) can be extended or needs a sibling. Finish the note with every result, and put any missed bar to the user.
  - verify: an Xvfb screenshot at 1920x1080 shows two prototype nodes joined port to port, sent to the user as one file. The note states pass or fail for every bar and a recommendation.
  - size: L

**Verification gate:** release gate green per `.local/build-mode`; `ShaderNode*_Test` pass; `tools/bench/shader_node_bench.py` meets every bar, or the user has ruled on each miss; the user has read the design note and confirmed the design before M350 - Shader Manifest And Nodes starts.

## Decisions

- 2026-11-05 — **Fixed bar, user rules on misses (user):** 50-node network; open and save under 2 s each; knob edit re-authors under 100 ms; comp edits no slower. A miss goes to the user, and only the user can leave the real-node design.
- 2026-11-05 — **Measured with knob-first nodes (user):** most attributes are knobs and only key and promoted ones are ports, so about 10 inputs per node is the case that must pass.
- 2026-11-05 — **Named outputs added (cat-plan):** Natron's connection model holds one output per node, and Cycles shaders have several.
- 2026-11-05 — **Lands bench, tests and note:** as a Feature member the spike needs a mergeable PR, and those keep measuring later work.
- 2026-11-17 — **Generic port-network core (user):** one network node class and one container class for shaders, VOPs and channels; nodes declare supported targets, and wiring checks targets and then port types. Chosen over shipping shader-only and refactoring later, which would have changed saved projects with no back-compat path.
- 2026-11-17 — **Node targets kept over a port-only check (user):** the alternative was type domains, where availability would follow from port types alone. The user kept node-level targets, so compatibility is a two-layer check.
- 2026-11-20 — Phase 349.1 T1–T3 in ddaab5f7b. T4's drawing prototype is scratch in build/m349-proto/ (not landed); it needs the build plus a screenshot. The real DwaBaseMaterial has 101 attributes, and a made-up 170-attribute layout is benched too. The design note is at Engine/Nodes/docs/shader-nodes.md, with its recommendation pending the numbers. Questions held for the user until the numbers exist: refusing bad connections in Node::connectInput, the 170 vs 101 record size, and a likely bug in NodeGroup::notifyNodeDeactivated's input rebuild.
- 2026-11-20 — ShaderNodeCostTest failed because the bench network wired MoonRay maps into Cycles BSDFs, which the target check rightly refuses. Fixed in 882a3894b (maps feed same-target materials round-robin; still 40 edges). main is merged into F2 (fast-forward) and into the lane (e5033c62e) for the AppImage packaging fix. The design note's 170-attribute top-end layout isn't built by Network::build; the note and the test are reconciled once numbers exist.
- 2026-11-20 — Gate green on 882a3894b (ShaderNodeCost, Inputs and Outputs tests pass). The bars' numbers and the drawing prototype's screenshot wait for the container, which is busy with M434's bench.
- 2026-11-22 — Prototype screenshot (scratch/m349-port-proto ce23c0e53) sent: port nodes with title bars and two port-to-port pipes. **User:** making NodeGui's positional arrows transparent is hacky; find a proper solution or confirm it architecturally — priority is a clean architecture to build on. Improve the look from prior art (VOP-style networks, ComfyUI, etc.): colour-coded port types, port groupings, possibly folding. A design proposal (architecture options with trade-offs, visuals, interaction) is being prepared for the user before any more prototype work.
