# M524 - Vertical Node Style

Add a third node draw style, Vertical, in the manner of Houdini's SOP networks: a compact body with the name beside it, inputs entering from the top, output leaving from the bottom, and chains growing downward. It joins Classic (M351 - Shader Graph Presentation's `RadialLayout`, today's look) and Ports (M351's `PortListLayout`/`PortNodeGui`, inputs left, named outputs right). It is a new `ConnectorLayout` (`VerticalLayout`) plus a `VerticalNodeGui`, so the one `Edge` class keeps taking endpoints and tangents from each end's layout (`PLAN/DECISIONS/2026-11-17-generic-port-networks.md`). The style is chosen per node family in preferences — Image, USD and Geometry — with Geometry defaulting to Vertical; port networks inside builders stay Ports only. Depends on M351 - Shader Graph Presentation (F2 - Native Shader Networks) and M522 - Geometry Toolbar Category (F15 - Native Geometry), so both Features land first; the user chose to wait for them rather than split the connector refactor out (2026-11-22). Houdini's network docs are in `.local/docs/houdini/network/` (`flags.md`, `wire.md`, `organize.md`, `options.md`); never mention Houdini in code, docs or tooltips.

## Scope

**In:**
- `VerticalLayout`: inputs as connectors along the top edge in input order; mask inputs enter from the right edge; output(s) along the bottom; body widens to fit connectors at a minimum spacing; variadic nodes always show one spare connector at the end.
- `VerticalNodeGui`: compact rounded body (about 80x24) with the plugin icon inside; name and the one-line info label to the right of the body; error and warning text beside the body under the name/label; today's state indicators (disabled, rendering, preview…) on the body as in Classic.
- Connector names shown on hover and while dragging a pipe near a connector.
- Node Graph preference `pipeShape` ("Pipe Shape": Straight / Bezier, default Bezier) for pipes touching a non-Classic node; bezier tangents from each end's layout. Classic↔Classic pipes stay straight.
- Preferences `imageNodeStyle` ("Image Node Style"), `usdNodeStyle` ("USD Node Style"), `geometryNodeStyle` ("Geometry Node Style"), each Classic / Ports / Vertical; defaults Classic, Classic, Vertical. Family from the node's domain category (`eNodeCategoryUsd3D` → USD, `eNodeCategoryNative3D` → Geometry, the rest → Image).
- Classic families in Ports style: a `PortListLayout` built from the node's inputs and its single output.
- Changing a style preference restyles every open graph live; node positions are untouched; style is not saved in projects.
- All main-graph gestures on Vertical and Ports nodes and on mixed pipes: drag-connect, kiss connect, shake disconnect, insert and splice by dropping on a pipe, Dots, bend points, selection, copy and paste.
- Auto-placement: a new node in Vertical style goes below its input, and downstream nodes below it.

**Out:**
- Vertical style for port networks inside builders: none; they stay Ports (user, 2026-11-22).
- Clickable side flags (bypass/display strips): none; existing indicators are reused (user, 2026-11-22).
- Per-node style overrides and a global default style: none (user, 2026-11-22: per family only).
- Re-spacing node positions when the style changes: none.
- Pipe width and stub lengths: M521 - Easier Pipe Grabbing (honoured here, not changed).
- Port groups and folding: M516 - Port Groups And Folding, Ports nodes in builders only.

**Edge cases:**
- Mixed pipes (Classic → Vertical, Vertical → Ports): each end uses its own layout's anchor and tangent; a Classic end's tangent is its radial direction.
- Bend points on a bezier pipe: the pipe is drawn as one bezier per segment through the bend points, tangents continuous at each bend.
- Unconnected inputs on a Vertical node: short stubs pointing up (masks: right), length from `disconnectedArrowLength`; unconnected output stub points down, length from `outputStubLength`.
- Inspector-style nodes (Viewer, Switch) in Vertical: all inputs on top, widened; no unfold.
- Dots and Backdrops: never restyled.
- Long names: the name beside the body is not truncated; it may overlap neighbours, as in Classic.
- A graph saved while laid out for Classic opens in Vertical with the same positions; overlap is expected.

**Assumptions:**
- M351's `PortNodeGui` can be fed a descriptor list built from a classic node's inputs; if not, Ports for classic families needs an adapter class.
- Unconfirmed: Ports-style auto-placement puts new downstream nodes to the right of their input.

## Phase 524.1: Vertical Node

- [ ] M524.P1.T1 — Add VerticalLayout
  - files: `Gui/VerticalLayout.{h,cpp}`, `Gui/CMakeLists.txt`, `Tests/VerticalLayout_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: a `ConnectorLayout` subclass (M351.P4.T2's interface: `inputAnchor(i)`, `outputAnchor(name)`, approach direction, hit test). Keep the geometry in a pure function taking body size, input count, which inputs are masks, whether the node is variadic, and output names, returning anchors, tangents and the widened body width, so it is unit-testable. Inputs spaced at a minimum spacing across the top (tangent up), masks down the right edge (tangent right), outputs across the bottom (tangent down), one spare connector after variadic inputs.
  - verify: gtest — 1, 2 and 10 inputs give evenly spaced top anchors and a body width that grows past the default at 10; a mask lands on the right edge; a variadic node gets one extra anchor; hit test finds the nearest connector and nothing outside its radius.
  - size: M

- [ ] M524.P1.T2 — Draw VerticalNodeGui
  - files: `Gui/VerticalNodeGui.{h,cpp}`, `Gui/NodeGraph.cpp` (`createNodeGUI` dispatch, ~line 659), `Gui/NodeGui.{h,cpp}`, `Gui/CMakeLists.txt`, `Tests/gui/vertical_node_style.py`
  - approach: a `NodeGui` subclass using `VerticalLayout`: compact rounded body with the plugin icon, name and info label to the right, error/warning text below them (reusing the text F29 - UI Polish Batch's node error display produces), Classic state indicators on the body, connector dots on the body edges with the input name as a hover tooltip-style label (also shown while a pipe is dragged near). Dispatch picks it when the node's family style is Vertical (a temporary hardcoded Geometry → Vertical until P3.T1 adds the preferences).
  - verify: an Xvfb script at 1920x1080 creates GeoTransform and GeoMerge (3 inputs), asserts the item type, body width growth with inputs, and the hover label text; screenshots of each node, plain and in error, go to the user, one file each.
  - size: L

## Phase 524.2: Pipes

- [ ] M524.P2.T1 — Bezier or straight pipes from layout tangents
  - files: `Engine/Settings.{h,cpp}`, `Gui/Edge.{h,cpp}`, `Gui/VerticalLayout.cpp`, `Tests/gui/vertical_node_pipes.py`
  - approach: add the `pipeShape` preference. When either end's layout is not a `RadialLayout`, `Edge` draws a cubic bezier from each end's anchor along its tangent (Bezier) or a straight line (Straight); with bend points, one bezier per segment, tangents continuous through each bend. Unconnected Vertical stubs follow the Edge cases, using M521's length and width preferences. Classic↔Classic pipes are untouched.
  - verify: the Xvfb script connects Classic → Vertical, Vertical → Vertical and a mask, toggles Pipe Shape, adds a bend point, and asserts each edge's path endpoints sit on the expected anchors; M351's `graph_connector_goldens.py` still passes; screenshots (Bezier and Straight) go to the user.
  - size: L

## Phase 524.3: Styles And Gestures

- [ ] M524.P3.T1 — Per-family style preferences with live restyle
  - files: `Engine/Settings.{h,cpp}`, `Gui/NodeGraph.cpp`, `Gui/NodeGui.cpp`, `Tests/gui/node_style_prefs.py`
  - approach: add the three style `KnobChoice`s on the Nodegraph page with the In defaults; a single function maps a node to its family and style (port networks inside a `NetworkBuilder` always Ports; Dots and Backdrops never restyled) and replaces P1.T2's hardcoded rule. On a change, rebuild the affected node items in every open graph keeping positions, selection and connections.
  - verify: the Xvfb script builds a graph with an image node, a USD node and a geometry node, switches each preference through all three styles, and asserts the item types, unchanged positions and intact connections after each switch; nothing changes in a builder's graph.
  - size: M

- [ ] M524.P3.T2 — Ports style for classic families
  - files: `Gui/PortNodeGui.cpp`, `Gui/PortListLayout.cpp`, `Gui/NodeGraph.cpp`, `Tests/gui/node_style_prefs.py`
  - approach: build the port list for a non-network node from its inputs (labels, mask flag) and its single output, and draw it with M351's `PortNodeGui` without port-type colours (one neutral colour) or promotion controls.
  - verify: the Xvfb script sets Image Node Style to Ports, asserts a Merge shows A, B and Mask ports with the right labels and its connections are drawn and editable; a screenshot goes to the user.
  - size: M

- [ ] M524.P3.T3 — Main-graph gestures on Vertical and Ports nodes
  - files: `Gui/NodeGraph10.cpp`, `Gui/NodeGraph*.cpp` (kiss, shake, insert/splice, Dot insertion), `Gui/Edge.cpp`, `Tests/gui/vertical_node_gestures.py`
  - approach: route every gesture's hit tests and drop targets through the nodes' `ConnectorLayout`s (M351.P4.T2 moved the classic ones there); make kiss connect pick the nearest top connector of a Vertical node, insert/splice and Dot insertion work on bezier pipes (hit test against the curve), and shake disconnect behave as on Classic nodes.
  - verify: the Xvfb script performs kiss connect, shake disconnect, drop-to-insert on a bezier pipe, Dot insertion, adding a bend point, and copy/paste on Vertical and mixed graphs, asserting the resulting connections; existing `m118_click_after_reset.py`, `m121_routing_mix.py` and `m159_group_kinds.py` still pass.
  - size: L

- [ ] M524.P3.T4 — Place Vertical nodes below their input
  - files: `Gui/NodeGraph*.cpp` (auto-placement on creation), `Tests/gui/vertical_node_gestures.py`
  - approach: when a node created connected to a selected node (toolbar, Tab, shortcut) is in Vertical style, place it below the upstream node's body plus its name/label height and the stub length; Ports style places it to the right; Classic is unchanged.
  - verify: the Xvfb script selects a GeoPrimitive, creates GeoTransform and GeoNormals from Tab, and asserts each lands below the previous with no overlap; a Classic image chain places as before.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `VerticalLayout_Test` passes; every `Tests/gui/vertical_node_*.py` and `node_style_prefs.py` pass under Xvfb at 1920x1080; M351 - Shader Graph Presentation's `graph_connector_goldens.py` passes unchanged; GUI bench rule (`tools/bench/gui/run.sh --xvfb` vs `baselines/xvfb.jsonl`) with no regression, recorded in `## Decisions`; the user OKs each screenshot.

## Decisions

- 2026-11-22 — **Wait for M351 (user):** the vertical style depends on M351 - Shader Graph Presentation's `ConnectorLayout` rather than splitting Phase 351.4 into its own milestone.
- 2026-11-22 — **Per-family style only (user):** Image, USD and Geometry preferences; no global default, no per-node override; Geometry defaults to Vertical.
- 2026-11-22 — **Look (user, grilling):** compact body with name beside; existing state indicators, with error/warning text beside the body under the name/label; masks enter from the right; connector names on hover; body widens for many inputs with a spare variadic connector.
- 2026-11-22 — **Pipe shape is a preference (user):** one Pipe Shape preference, Bezier by default, for pipes touching non-Classic nodes.
- 2026-11-22 — **All gestures, live restyle, placement below (user):** every main-graph gesture works on Vertical; style changes restyle open graphs keeping positions; Vertical nodes auto-place below their input.
- 2026-11-22 — **Port networks stay Ports (user):** no Vertical inside builders.
