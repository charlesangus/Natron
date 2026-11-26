# M351 - Shader Graph Presentation

Give port networks their presentation inside a builder's graph tab: left-to-right nodes with type-coloured ports, bezier pipes, named outputs, typed connection dragging and a Tab menu limited to one target. Ports are the single connection model (M349 - Shader Node Spike's 2026-11-22 decisions): each `NodeGui` owns a `ConnectorLayout` (`inputAnchor(i)`, `outputAnchor(name)`, approach direction, hit test) and the one `Edge` class takes its endpoints and tangents from it. Classic nodes get a `RadialLayout` that reproduces today's angles exactly, landed first behind pixel-identical graph goldens; port nodes get a `PortListLayout`, drawn by a `PortNodeGui` subclass dispatched in `Gui/NodeGraph.cpp:468` beside `DotGui` and `BackdropGui`. There is no sibling `PortEdge` and no hidden-arrow trick. Graph items read Node inputs plus the port descriptors from M350 - Shader Manifest And Nodes (M350.P2.T6). A `USDMaterialBuilder` shell (a `NodeGroup` subclass with no authoring) lands here so that the graph has a real home; M352 - USDMaterialBuilder Node gives it its output and authoring. Every piece is generic (`PLAN/DECISIONS/2026-11-17-generic-port-networks.md`): `ConnectorLayout`, `PortNodeGui`, `PortTypes` and a `NetworkBuilder` base holding a set of targets. Shader targets are the first client; M482 - Geometry VOP Node and F32 - Channel Networks reuse it unchanged, so nothing here names shaders or renderers outside the shader target registrations. The design proposal M349 - Shader Node Spike's PM wrote is at `.worktrees/m349/build/m349-port-design.md` on the dev host; this file carries everything a task needs from it.

## Scope

**In:**
- `ConnectorLayout` and a `RadialLayout` holding today's angle logic, including masks, side inputs and the inspector unfold, with classic graph goldens pixel-identical before and after.
- `PortListLayout`: input ports down the left, named output ports down the right.
- Port look from a core type table: float light grey, int lime, vector3 dark blue, normal and point blue-violet, color yellow, string light blue, closure bright green, displacement teal, texture apricot, channel magenta, custom white with a coloured ring. Circles for single values, pills for arrays, diamonds for varying values.
- Header colour by node category, reusing Natron's per-group colours, plus a renderer stripe.
- Read-only value summaries (a swatch or a number) beside unconnected promoted ports.
- Bezier pipes with horizontal tangents, coloured by the source port's type, thicker for arrays, a two-colour gradient where a type conversion happens.
- Default content: promoted ports (key ports, or every linkable attribute for shaders with 12 ports or fewer) plus port-only attributes.
- Connecting (user, 2026-11-22): clicking an output port picks up a new pipe (outputs fan out); clicking an input port picks up its pipe, or a new one when unconnected; clicking a pipe picks it up by the end nearer the click, as on today's graph. Dropping on a port connects; dropping on empty space disconnects; shift-dropping on empty space opens the target-filtered Tab menu, and the new node's compatible ports (inputs or outputs, by the end being placed) are highlighted, the first is picked, and scroll or up/down moves the pipe between them. Incompatible ports dim to about 30%, compatible ones get a halo, and a refused drop shows its reason in the status bar ("no shared target", "closure cannot feed color3").
- Promote and demote control beside each promotable knob in the panel; a connected port's knob greys out.
- Tab lists the nodes of one of the container's targets plus Backdrop; Shift+Tab switches target. A builder's targets are Cycles and MoonRay.
- Gestures inside: selection, copy and paste, delete, undo, Backdrops. A `MissingPluginNode` draws with ports from its stored inputs.

**Out:**
- Port groups, fold states, per-port hide and show, drop-on-node attribute search and the filtered node menu on an empty drop: M516 - Port Groups And Folding.
- Inline value widgets on nodes: never (user, 2026-11-22).
- Dots and typed reroutes inside port graphs: none yet.
- Kiss connect, shake disconnect, insert and splice inside port graphs: off (M194 - Kiss Connections, M195 - Shake And Cut Disconnect, M277 - Graph Creation And Insert keep the main graph).
- Bend points on bezier pipes: none.
- Nested sub-networks: M482 - Geometry VOP Node adds subnets. `NetworkBuilder` must stay nestable inside itself; nothing here may assume a top-level container.
- Viewing an intermediate shader or a shader ball: none.
- Any visible change to main-graph drawing: none; the `RadialLayout` refactor is pixel-identical.

**Edge cases:**
- Classic graph features the refactor must keep: mask inputs, side inputs, the inspector unfold (`unfoldsInputs`, `Gui/NodeGui.cpp:2125`), output edges, bend points, splice hints, Dots, selection and rendering colours, data-kind pens.
- Demoting a connected port: asks before disconnecting.
- Connecting to an occupied input: replaces the connection, as elsewhere in Natron.
- Non-network nodes pasted or created by script inside: refused by the kind check, with the usual message.
- 1920x1080: a fully promoted `DwaBaseMaterial` exceeds the screen, so the graph scrolls and zooms as usual.

**Assumptions:**
- Splice, bend points and Dot insertion were never prototyped with bezier pipes; they stay classic-only here.

## Phase 351.4: Connector Layout

- [x] M351.P4.T1 — Record classic graph goldens
  - files: `Tests/gui/graph_connector_goldens.py`, `Tests/fixtures/graph_connectors/`
  - approach: on unchanged main, build small graphs that cover every classic connector case: a Merge with A, B and mask; a node with side inputs; a Viewer and a Switch unfolded as inspectors; an output edge; a bend point; a Dot chain; selected, highlighted and rendering-coloured edges; each data-kind pen. Render each graph's scene region to a PNG at a fixed zoom and compare exactly. Keep each golden a small crop (≤256 px a side) per the LFS rule. Record them in the published image per the GUI golden recipe.
  - verify: the script passes twice in a row on main; editing one edge's angle by hand in a scratch build makes it fail naming the case.
  - size: M

- [ ] M351.P4.T2 — Add ConnectorLayout and move classic anchors into RadialLayout
  - files: `Gui/ConnectorLayout.{h,cpp}`, `Gui/RadialLayout.{h,cpp}`, `Gui/Edge.{h,cpp}`, `Gui/NodeGui.{h,cpp}`, `Gui/NodeGraph10.cpp`, `Gui/CMakeLists.txt`
  - approach: `ConnectorLayout` gives `inputAnchor(i)`, `outputAnchor(name)`, each anchor's approach direction (tangent) and a hit test for the input or output under a point. Move today's angle logic out of `NodeGui::initializeInputs`, `initializeInputsForInspector` and `Edge::setAngle` into `RadialLayout`, reproducing the mask, side-input and inspector rules exactly. `Edge::initLine` asks the destination's layout for the anchor and tangent, and the source's for the output anchor. The 46 `Edge` uses in `NodeGraph10.cpp` (drag, splice, hit tests) go through the layout. No visible change.
  - verify: `graph_connector_goldens.py` passes unchanged; `m118_click_after_reset.py`, `m121_routing_mix.py` and `m159_group_kinds.py` pass under Xvfb; the graph ctest cases pass.
  - size: L

## Phase 351.1: Port Nodes

- [x] M351.P1.T4 — Add the port type look table
  - files: `Engine/Nodes/Network/PortTypes.{h,cpp}`, `Tests/Network/PortTypes_Test.cpp`
  - approach: a core table keyed by M350.P2.T6's type classes giving each its display colour (the In list) and, from the descriptor's array and varying flags, its shape: circle, pill or diamond. Custom types get white with a ring colour derived from their type name. Nothing here decides compatibility; M351.P2.T1 adds that to the same class.
  - verify: gtest — every type class has a colour; array gives pill, varying gives diamond, otherwise circle; two custom type names get different ring colours.
  - size: M

- [ ] M351.P1.T1 — Add the builder shell and port node drawing
  - files: `Engine/Nodes/Network/NetworkBuilder.{h,cpp}`, `Engine/Nodes/Scene/USDMaterialBuilder.{h,cpp}`, `Gui/PortNodeGui.{h,cpp}`, `Gui/PortListLayout.{h,cpp}`, `Gui/NodeGraph.cpp`, `Tests/gui/shader_graph_nodes.py`
  - approach: `NetworkBuilder`, a `NodeGroup` subclass with `isSubGraphUserVisible` true, a target set, and dive-in to its own tab; `USDMaterialBuilder` derives from it with targets `cycles` and `moonray` and a Scene output. Dispatch `PortNodeGui` for network nodes; it uses a `PortListLayout` built from the port descriptors, inputs left and named outputs right. Draw the header in the category colour with the renderer stripe, the title, each port's shape and colour from `PortTypes`, labels, and read-only summaries beside unconnected promoted ports. Show promoted and port-only ports. Laid out left to right.
  - verify: an Xvfb script at 1920x1080 creates a builder, dives in, and creates Principled BSDF and `DwaBaseMaterial`; it asserts the visible port count matches each node's promoted plus port-only attributes; screenshots go to the user, one file each.
  - size: L

- [ ] M351.P1.T2 — Draw bezier pipes between ports
  - files: `Gui/Edge.{h,cpp}`, `Gui/PortListLayout.cpp`, `Gui/PortNodeGui.cpp`, `Tests/gui/shader_graph_edges.py`
  - approach: when both ends use a `PortListLayout`, `Edge` draws a cubic bezier with horizontal tangents from the layout, coloured by the source port's type, thicker for arrays, and a two-colour gradient where source and destination type classes differ. The downstream node records the output name. No per-input edge exists for an unconnected port. Bend points and splice hints are not shown on pipes.
  - verify: an Xvfb script connects the Color and Alpha outputs of one image texture to two inputs and a float output to a color input, moves nodes, saves and reloads, and asserts the same connections and output names; `graph_connector_goldens.py` still passes; a screenshot goes to the user.
  - size: L

- [ ] ~~M351.P1.T3 — Collapse groups on the node~~ — cancelled: moved to M516 - Port Groups And Folding (M349 - Shader Node Spike decision, 2026-11-22).
  - files: `Gui/PortNodeGui.cpp`, `Engine/Nodes/Network/NetworkNode.cpp`, `Tests/gui/shader_graph_groups.py`
  - approach: group rows from the manifest, holding the node's visible ports. A collapsed row shrinks the node, and edges to its ports land on the row. Saved with the node.
  - verify: an Xvfb script collapses a group with a connected port, asserts the edge end sits on the row, saves, reloads and asserts it's still collapsed; a screenshot goes to the user.
  - size: M

## Phase 351.2: Connecting

- [ ] M351.P2.T1 — Typed connection dragging
  - files: `Engine/Nodes/Network/PortTypes.{h,cpp}`, `Gui/PortNodeGui.cpp`, `Gui/NodeGraph10.cpp`, `Tests/Network/PortTypes_Test.cpp`, `Tests/gui/shader_graph_drag.py`
  - approach: `PortTypes` answers whether an output port can connect to an input port, with a reason when it can't. Each target registers its implicit conversions (Cycles' and MoonRay's here); ports of nodes sharing no target never connect. Clicking an output port picks up a new pipe; clicking an input port picks up its pipe, or a new one when unconnected; clicking a pipe picks it up by the nearer end. Dropping on a port connects; dropping on empty space disconnects. During a drag incompatible ports dim to about 30% and compatible ones get a halo. A refused drop changes nothing and shows the reason in the status bar. Kiss, shake, splice and insert don't run inside port graphs.
  - verify: gtest — float to color on Cycles allowed, closure to float refused with "closure cannot feed float", Cycles to MoonRay refused with "no shared target"; an Xvfb script drags onto a compatible and an incompatible port, picks a pipe up by each end, drops one on empty space (disconnected), and asserts the status-bar text; a screenshot of the dimming goes to the user.
  - size: M

- [ ] M351.P2.T2 — Promote from the panel
  - files: `Gui/KnobGui.cpp`, `Gui/KnobGui.h`, `Tests/gui/shader_graph_promote.py`
  - approach: a small control beside each promotable knob, shown only on shader nodes. A connected port's knob greys out, and demoting a connected port asks first.
  - verify: an Xvfb script promotes an attribute, connects it, sees the knob disabled, demotes it through the question and sees the knob enabled; a screenshot goes to the user.
  - size: M

- [ ] ~~M351.P2.T3 — Drop list of compatible attributes~~ — cancelled: moved to M516 - Port Groups And Folding (M349 - Shader Node Spike decision, 2026-11-22).
  - files: `Gui/PortAttributeList.{h,cpp}`, `Gui/PortNodeGui.cpp`, `Tests/gui/shader_graph_drop_list.py`
  - approach: on dropping a connection on a node's body, open a popup grouped by attribute group, with a filter field, listing only compatible attributes including hidden ones. Picking promotes and connects as one undo step.
  - verify: an Xvfb script drops on `DwaBaseMaterial`, filters to one attribute, picks it, asserts the port and connection, then undoes once and asserts both are gone; a screenshot goes to the user.
  - size: M

## Phase 351.3: Menu

- [ ] M351.P3.T1 — Limit Tab to one target
  - files: `Gui/TabMenuBridge.cpp`, `Gui/NodeGraph25.cpp`, `Tests/gui/shader_graph_tab.py`
  - approach: inside a builder's graph, Tab lists the nodes supporting the current target plus Backdrop. Shift+Tab switches among the container's targets, with the menu showing which is current. The main graph's Tab offers `USDMaterialBuilder` and no shader.
  - verify: an Xvfb script opens Tab in both graphs and asserts the entries; Shift+Tab swaps shader lists; a screenshot goes to the user.
  - size: M

- [ ] M351.P3.T2 — Draw missing shaders with ports
  - files: `Gui/NodeGraph.cpp`, `Gui/PortNodeGui.cpp`, `Tests/gui/shader_graph_missing.py`
  - approach: a `MissingPluginNode` inside a shader graph draws with its stored inputs as ports, marked with Natron's usual missing look, so its connections stay visible.
  - verify: an Xvfb script loads a project naming an unknown shader, asserts its connections are drawn and that saving keeps them; a screenshot goes to the user.
  - size: M

- [ ] M351.P3.T3 — Shift-drop creates a wired node
  - files: `Gui/NodeGraph10.cpp`, `Gui/TabMenuBridge.cpp`, `Gui/PortNodeGui.cpp`, `Tests/gui/shader_graph_shift_drop.py`
  - approach: shift-dropping a picked-up pipe on empty space opens the Tab menu filtered to nodes with a shared target and a type-compatible port (via `PortTypes`). The created node appears at the drop point with its compatible ports highlighted — inputs when the pipe's output end is held, outputs when its input end is — the first one connected, and scroll or up/down move the connection to the next or previous compatible port until the user clicks or presses Enter/Escape. Escape cancels the creation and drops the pipe as a disconnect. One undo step covers the creation and the wiring. Written against `ConnectorLayout` so M550 - Classic Graph Shift-Drop Creation can reuse it on radial nodes.
  - verify: an Xvfb script shift-drops a closure pipe, picks a BSDF, cycles with the down key, and asserts the final input; undo removes node and pipe; a screenshot of the highlight goes to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `PortTypes_Test` passes; `graph_connector_goldens.py` and every `Tests/gui/shader_graph_*.py` pass under Xvfb at 1920x1080; the user OKs each screenshot.

## Decisions

- 2026-11-05 — **Real nodes in a Group with a new drawing (user):** option A of the VOP-like analysis, subject to M349 - Shader Node Spike.
- 2026-11-05 — **Knob-first with promotion and a drop list (user):** most attributes are knobs; promote from the panel, or drop on the node and pick from a filterable list.
- 2026-11-05 — **Key ports plus collapsible groups on the node (user):** "have groups to manage complexity". Taken as group rows on the node as well as in the panel and list; a collapsed group's edges land on its row (cat-plan).
- 2026-11-05 — **Minimal gestures at first (user):** port drag, selection, copy and paste; kiss and shake later if missed.
- 2026-11-17 — **Generic presentation (user):** port node, edge, type registry and builder base are context-neutral so VOP and channel containers reuse them; shader-specific code lives only in the shader target registrations.
- 2026-11-22 — **Ports as the single connection model (user, via M349 - Shader Node Spike):** `ConnectorLayout` per `NodeGui`, classic nodes refactored first to a pixel-identical `RadialLayout`, port nodes on `PortListLayout`; replaces the sibling `PortEdge`. Phase 351.4 was added for the refactor and runs first; it is numbered 351.4 because phase and task IDs are never renumbered (cat-plan).
- 2026-11-22 — **Look as proposed (user):** type colours, shapes, bezier pipes, compatible-port feedback with a status-bar reason, category header colours, read-only summaries, no inline widgets. Groups, folding and search-to-connect move to M516 - Port Groups And Folding, which stays in F2 - Native Shader Networks (user, 2026-11-22).
- 2026-11-22 — **Connection gestures (user):** click-to-pick-up on ports and pipes, empty drop disconnects, shift-drop opens Tab and cycles the new node's compatible ports; replaces the earlier "drop on empty space opens the menu" and ctrl-drag fan-out. The same gesture comes to the classic graph and plain Tab creation in M550 - Classic Graph Shift-Drop Creation. Design note updated in d518cbe66.
- 2026-11-26 (PM) — Claimed (bosley-executor); lane `.worktrees/m351` on `milestone/m351-shader-graph-presentation` off F2 after M350 merged. P4.T1 script in 472523d803: 14 cases (Merge A/B/mask, side inputs, Viewer and Switch inspectors, output edge, bend point, Dot chain, selected and cut-highlighted edges, image/deep/scene/raw/polymorphic pens) rendered at scale 0.75 into 256 px crops. No GUI golden recorder existed, so the script records a missing golden (or all with `GUI_TEST_RECORD_GOLDENS=1`) and copies recordings to its output dir. Not covered: rendering-coloured edges (transient, can't be set from Python), a geometry pen (no geometry data kind), network pens (need a builder; later). m351-gate1 runs it in the published image to record. P1.T4 done in the next commit: `PortTypes` colours and shapes (varying beats array), FNV-1a ring hue for custom types, four gtests.
- 2026-11-26 (PM) — P4.T1 done: 14 goldens recorded in the published image (m351-gate1, plus output_edge on gate2 after it was shortened to two nodes); m351-gate2 re-ran the script on the same build and all 13 committed goldens matched exactly (92 checks, 0 failures). P4.T2 dispatched.
- 2026-11-26 (PM) — P4.T2 implemented in 88069e23f0: `ConnectorLayout` (anchors with tangent and clip outline, `hitTest`, `hitEdge`) owned by every NodeGui; `RadialLayout` holds the angle logic moved verbatim from `NodeGui::initializeInputs`/`initializeInputsForInspector` and `Edge::setAngle`; Edge takes anchors from the layouts; NodeGraph10's edge picking asks the layouts. Drag/splice math in NodeGraph15/20 computes no anchors and is unchanged. m351-gate3 (goldens + m118/m121/m159 + ctest) queued. P1.T1-T2 dispatched as one unit.
- 2026-11-26 (PM) — P1.T1-T2 implemented in the next commit: `NetworkBuilder` (NodeGroup, targets, user-visible sub-graph) and `USDMaterialBuilder` (cycles, moonray; Scene output; no authoring); `PortNodeGui` on `PortListLayout` (header in the node colour with a renderer stripe split across targets, typed port items, read-only summaries); pipes are cubic beziers between two port layouts, coloured by the source type, doubled for arrays, gradient across type classes, no arrow/label/bend/splice hints; dragged ends fall back to straight lines; low-detail zoom draws straight lines. m351-gate4 (goldens, m118/m121/m159, shader_graph_nodes/edges) queued; screenshots go to the user.
