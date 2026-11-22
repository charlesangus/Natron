# M516 - Port Groups And Folding

Make large port nodes manageable inside a builder's graph: collapsible port groups that mirror the knob pages, three fold states per node, per-port hide and show, and search-to-connect, both when dropping a pipe on a node and when dropping it on empty space. Builds on M351 - Shader Graph Presentation's `PortNodeGui`, `PortListLayout`, `PortTypes` and typed dragging, and on the port descriptors' pages from M350 - Shader Manifest And Nodes (M350.P2.T6). Generic like the rest of the port-network presentation (`PLAN/DECISIONS/2026-11-17-generic-port-networks.md`): nothing names shaders or renderers. A member of F2 - Native Shader Networks (user, 2026-11-22).

## Scope

**In:**
- Group rows on the node, one per descriptor page (Base, Specular, Coat, Emission and so on), collapsible; a collapsed group's pipes land on its row. Expanded state saves with the node.
- Three fold states: all promoted ports, connected only, header only. Default: connected only for nodes with more than 20 ports, otherwise all promoted. Header only is set by hand.
- Per-port hide and show.
- While a pipe is dragged, a folded or collapsed node shows its compatible ports.
- Dropping a pipe on a node's body opens a filterable list of its compatible bindable attributes, grouped by page, including unpromoted ones; picking one promotes and connects it as one undo step.
- Dropping a pipe on empty space opens the Tab menu filtered to nodes that share a target and have a type-compatible port; the new node is created there and wired.

**Out:**
- Inline value widgets: never (user, 2026-11-22).
- Typed Dots and reroutes: none yet.
- Main-graph behaviour: none; classic nodes have no groups or fold states.

**Edge cases:**
- Collapsing a group, or folding a node, with connected ports: pipes land on the row or the header, and the connection is kept.
- Hiding a connected port: refused, with a status-bar message; disconnect first.
- Dropping on a node with no compatible attribute: the list says so and connects nothing.
- Empty-space drop where no node qualifies: the menu says so and creates nothing.
- A node whose port count crosses 20 after promotions keeps its saved fold state; the default applies only at creation.

## Phase 516.1: Groups

- [ ] M516.P1.T1 — Collapse port groups on the node
  - files: `Gui/PortNodeGui.cpp`, `Gui/PortListLayout.cpp`, `Engine/Nodes/Network/NetworkNode.{h,cpp}`, `Tests/gui/port_graph_groups.py`
  - approach: build group rows from the descriptors' pages, holding each page's visible ports. A collapsed row shrinks the node, and `PortListLayout` returns the row's position as the anchor of each port under it, so pipes land there. Expanded state is a hidden knob on the node, saved with it and undoable.
  - verify: an Xvfb script collapses a group holding a connected port, asserts the pipe end sits on the row, saves, reloads and asserts it is still collapsed and connected; undo expands it; a screenshot goes to the user.
  - size: M

## Phase 516.2: Folding

- [ ] M516.P2.T1 — Add the three fold states
  - files: `Gui/PortNodeGui.cpp`, `Gui/PortListLayout.cpp`, `Engine/Nodes/Network/NetworkNode.{h,cpp}`, `Tests/gui/port_graph_fold.py`
  - approach: a fold state per node (all promoted, connected only, header only) in a hidden saved knob, cycled from a header button and the node's context menu, undoable. Set the default at creation: connected only above 20 ports, otherwise all promoted. In header only, pipes land on the header's sides.
  - verify: an Xvfb script creates `DwaBaseMaterial` (connected only by default) and a small Cycles node (all promoted), cycles a fold state, saves and reloads, and asserts the states and that every connection is still drawn; screenshots go to the user, one file each.
  - size: M

- [ ] M516.P2.T2 — Hide and show single ports
  - files: `Gui/PortNodeGui.cpp`, `Engine/Nodes/Network/NetworkNode.{h,cpp}`, `Tests/gui/port_graph_hide.py`
  - approach: a port's context menu hides it, and the node's context menu lists hidden ports to show again. The hidden set saves with the node. Hiding a connected port is refused with a status-bar message.
  - verify: an Xvfb script hides an unconnected port, saves, reloads, asserts it is hidden, shows it again; trying to hide a connected port leaves it shown and sets the status-bar message.
  - size: M

- [ ] M516.P2.T3 — Reveal compatible ports while dragging
  - files: `Gui/PortNodeGui.cpp`, `Gui/NodeGraph10.cpp`, `Tests/gui/port_graph_reveal.py`
  - approach: while a pipe is dragged, a node under the cursor that is folded, collapsed or hiding ports temporarily lists its compatible ports, using `PortTypes`' check, and restores its look when the drag leaves or ends.
  - verify: an Xvfb script drags a closure output over a header-only `DwaBaseMaterial`, asserts its compatible ports appear, drops on one, and asserts the connection and that the node returns to header only; a screenshot goes to the user.
  - size: M

## Phase 516.3: Search To Connect

- [ ] M516.P3.T1 — Drop list of compatible attributes
  - files: `Gui/PortAttributeList.{h,cpp}`, `Gui/PortNodeGui.cpp`, `Tests/gui/port_graph_drop_list.py`
  - approach: on dropping a pipe on a node's body, open a popup grouped by page, with a filter field, listing only compatible bindable attributes, including unpromoted and hidden ones. Picking promotes (M350.P2.T4) and connects as one undo step.
  - verify: an Xvfb script drops on `DwaBaseMaterial`, filters to one attribute, picks it, asserts the port and connection, then undoes once and asserts both are gone; a drop on a node with no compatible attribute connects nothing; a screenshot goes to the user.
  - size: M

- [ ] M516.P3.T2 — Filtered node menu on an empty drop
  - files: `Gui/TabMenuBridge.cpp`, `Gui/NodeGraph10.cpp`, `Gui/NodeGraph25.cpp`, `Tests/gui/port_graph_empty_drop.py`
  - approach: dropping a pipe on empty space inside a builder opens the Tab menu filtered to nodes that share a target with the pipe's node and have a type-compatible port, using the manifest's descriptors without creating nodes. Picking one creates it at the drop point and wires the first compatible port, as one undo step.
  - verify: an Xvfb script drags a Cycles closure output to empty space, asserts the menu lists only Cycles nodes with a closure input, picks one, and asserts the wiring; undo removes node and connection; a screenshot goes to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; every `Tests/gui/port_graph_*.py` and M351 - Shader Graph Presentation's `shader_graph_*.py` pass under Xvfb at 1920x1080; the user OKs each screenshot.

## Decisions

- 2026-11-22 — **Split out of M351 - Shader Graph Presentation (user, via M349 - Shader Node Spike):** groups, folding and search-to-connect come after the port-node presentation; default fold and look as the user approved on 2026-11-22. Kept in F2 - Native Shader Networks (user).
