# M351 - Shader Graph Presentation

Give shader networks their own presentation inside a builder's graph tab: left-to-right nodes with labelled, colour-coded ports, typed connection dragging and a Tab menu limited to one renderer. The main graph doesn't change. It is drawn by a new `NodeGui` subclass dispatched in `Gui/NodeGraph.cpp:468` beside `DotGui` and `BackdropGui`, with a port-to-port edge per M349 - Shader Node Spike's result. A `USDMaterialBuilder` shell (a `NodeGroup` subclass with no authoring) lands here, so that the shader graph has a real home; M352 - USDMaterialBuilder Node gives it its output and authoring.

## Scope

**In:**
- Shader node drawing: title with a renderer colour stripe, named output ports on the right, input ports on the left.
- Default content: key ports, or every linkable attribute for shaders with 12 ports or fewer, plus promoted and port-only attributes.
- Collapsible group rows on the node. Promoted ports sit under their group, and a collapsed group's edges land on its row. Expanded state saves with the node.
- Type colour per port. While dragging, incompatible ports grey out and can't accept. Compatibility follows each renderer's real rules, including Cycles' implicit conversions, and bars crossing renderers.
- Promote and demote control beside each promotable knob in the panel; a connected port's knob greys out.
- Dropping a connection anywhere on a node opens a filterable list of its compatible attributes, grouped; picking one promotes and connects it.
- Tab lists one renderer's shaders, starting with the default, plus Backdrop. Shift+Tab switches renderer.
- Gestures inside: drag a port to connect, selection, copy and paste, delete, undo, Backdrops. A `MissingPluginNode` draws with ports from its stored inputs.

**Out:**
- Kiss connect, shake disconnect and insert: off inside shader graphs for now (M194 - Kiss Connections, M195 - Shake And Cut Disconnect, M277 - Graph Creation And Insert keep the main graph).
- Dots inside shader graphs, which would need typed pass-through: none yet.
- Nested sub-networks: none yet.
- Viewing an intermediate shader or a shader ball: none.
- Any change to main-graph drawing: none.

**Edge cases:**
- Demoting a connected port: asks before disconnecting.
- Connecting to an occupied input: replaces the connection, as elsewhere in Natron.
- Dropping on a node with no compatible attribute: the list says so and connects nothing.
- Non-shader nodes pasted or created by script inside: refused by the kind check, with the usual message.
- 1920x1080: a fully expanded `DwaBaseMaterial` exceeds the screen, so the graph scrolls and zooms as usual.

**Assumptions:**
- The port edge can use `QGraphicsItem` machinery like `Edge`. M349 - Shader Node Spike's note decides between extension and a sibling class.

## Phase 351.1: Drawing

- [ ] M351.P1.T1 — Add the builder shell and shader node drawing
  - files: `Engine/Nodes/Scene/USDMaterialBuilder.{h,cpp}`, `Gui/ShaderNodeGui.{h,cpp}`, `Gui/NodeGraph.cpp`, `Tests/gui/shader_graph_nodes.py`
  - approach: a `NodeGroup` subclass with `isSubGraphUserVisible` true, a Scene output and dive-in to its own tab. Dispatch `ShaderNodeGui` for shader nodes. Draw stripe, title, ports, labels and type colours from the manifest. Laid out left to right.
  - verify: an Xvfb script at 1920x1080 creates a builder, dives in, and creates Principled BSDF and `DwaBaseMaterial`; screenshots go to the user, one file each.
  - size: L

- [ ] M351.P1.T2 — Draw port-to-port edges
  - files: `Gui/ShaderEdge.{h,cpp}` or `Gui/Edge.{h,cpp}` per the spike, `Gui/ShaderNodeGui.cpp`, `Tests/gui/shader_graph_edges.py`
  - approach: edges run from a named output to a named input port with type colour, and follow node moves. Saved projects restore them, including named outputs. No per-input edge is created for an unconnected port.
  - verify: an Xvfb script connects Color and Alpha outputs of one image texture to two inputs, moves nodes, saves and reloads, and asserts the same connections; a screenshot goes to the user.
  - size: L

- [ ] M351.P1.T3 — Collapse groups on the node
  - files: `Gui/ShaderNodeGui.cpp`, `Engine/Nodes/Scene/Shaders/ShaderNode.cpp`, `Tests/gui/shader_graph_groups.py`
  - approach: group rows from the manifest, holding the node's visible ports. A collapsed row shrinks the node, and edges to its ports land on the row. Saved with the node.
  - verify: an Xvfb script collapses a group with a connected port, asserts the edge end sits on the row, saves, reloads and asserts it's still collapsed; a screenshot goes to the user.
  - size: M

## Phase 351.2: Connecting

- [ ] M351.P2.T1 — Typed connection dragging
  - files: `Gui/ShaderNodeGui.cpp`, `Gui/NodeGraph10.cpp`, `Engine/Nodes/Scene/Shaders/ShaderTypes.{h,cpp}`, `Tests/Scene/ShaderTypes_Test.cpp`
  - approach: `ShaderTypes` answers whether an output can connect to an input, per renderer rules, and bars crossing renderers. The drag greys incompatible ports. A refused drop leaves things unchanged. Kiss, shake and insert don't run inside shader graphs.
  - verify: gtest — float to colour on Cycles allowed, closure to float refused, Cycles to MoonRay refused; an Xvfb script drags onto a compatible and an incompatible port, with a screenshot of greying to the user.
  - size: M

- [ ] M351.P2.T2 — Promote from the panel
  - files: `Gui/KnobGui.cpp`, `Gui/KnobGui.h`, `Tests/gui/shader_graph_promote.py`
  - approach: a small control beside each promotable knob, shown only on shader nodes. A connected port's knob greys out, and demoting a connected port asks first.
  - verify: an Xvfb script promotes an attribute, connects it, sees the knob disabled, demotes it through the question and sees the knob enabled; a screenshot goes to the user.
  - size: M

- [ ] M351.P2.T3 — Drop list of compatible attributes
  - files: `Gui/ShaderAttributeList.{h,cpp}`, `Gui/ShaderNodeGui.cpp`, `Tests/gui/shader_graph_drop_list.py`
  - approach: on dropping a connection on a node's body, open a popup grouped by attribute group, with a filter field, listing only compatible attributes including hidden ones. Picking promotes and connects as one undo step.
  - verify: an Xvfb script drops on `DwaBaseMaterial`, filters to one attribute, picks it, asserts the port and connection, then undoes once and asserts both are gone; a screenshot goes to the user.
  - size: M

## Phase 351.3: Menu

- [ ] M351.P3.T1 — Limit Tab to one renderer
  - files: `Gui/TabMenuBridge.cpp`, `Gui/NodeGraph25.cpp`, `Tests/gui/shader_graph_tab.py`
  - approach: inside a builder's graph, Tab lists the current renderer's shaders plus Backdrop. Shift+Tab switches renderer, with the menu showing which is current. The main graph's Tab offers `USDMaterialBuilder` and no shader.
  - verify: an Xvfb script opens Tab in both graphs and asserts the entries; Shift+Tab swaps shader lists; a screenshot goes to the user.
  - size: M

- [ ] M351.P3.T2 — Draw missing shaders with ports
  - files: `Gui/NodeGraph.cpp`, `Gui/ShaderNodeGui.cpp`, `Tests/gui/shader_graph_missing.py`
  - approach: a `MissingPluginNode` inside a shader graph draws with its stored inputs as ports, marked with Natron's usual missing look, so its connections stay visible.
  - verify: an Xvfb script loads a project naming an unknown shader, asserts its connections are drawn and that saving keeps them; a screenshot goes to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `ShaderTypes_Test` passes; every `Tests/gui/shader_graph_*.py` passes under Xvfb at 1920x1080; the user OKs each screenshot.

## Decisions

- 2026-11-05 — **Real nodes in a Group with a new drawing (user):** option A of the VOP-like analysis, subject to M349 - Shader Node Spike.
- 2026-11-05 — **Knob-first with promotion and a drop list (user):** most attributes are knobs; promote from the panel, or drop on the node and pick from a filterable list.
- 2026-11-05 — **Key ports plus collapsible groups on the node (user):** "have groups to manage complexity". Taken as group rows on the node as well as in the panel and list; a collapsed group's edges land on its row (cat-plan).
- 2026-11-05 — **Minimal gestures at first (user):** port drag, selection, copy and paste; kiss and shake later if missed.
