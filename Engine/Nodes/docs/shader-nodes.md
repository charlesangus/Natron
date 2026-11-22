# Port nodes: shader, kernel and channel networks

Real Natron nodes as network nodes: one generic port-node class and one container class serve
shaders, kernel networks and channel networks. Each node declares the targets it supports and its
typed ports; wiring checks targets first, then port types. The evidence here comes from a test
effect, `PortTestNode` (`Engine/Nodes/Test/`), the `ShaderNode*_Test` gtests and
`tools/bench/shader_node_bench.py`.

## Measurements

Network: 50 nodes in a Group — 5 large materials (one at 101 attributes, the real
`DwaBaseMaterial` count, and a 170-attribute top-end layout), 10 BSDFs (35 attributes), 5
two-output image textures and 30 small maps — with 40 edges, about 10 ports per node. Knob edits
re-author the whole network as UsdShade shaders (every attribute value and connection) on an
anonymous layer. Comp edits are a 20-Grade chain: a knob change plus a reconnection per round.

| Bar | Limit | Knob-first | Every attribute a port | Result |
|---|---|---|---|---|
| Open | < 2 s | _pending_ | _pending_ | _pending_ |
| Save | < 2 s | _pending_ | _pending_ | _pending_ |
| Knob edit re-authors | < 100 ms | _pending_ | _pending_ | _pending_ |
| Comp edits (in process) | no slower | _pending_ | _pending_ | _pending_ |
| Comp edits (graph_bench re-render) | no slower | _pending_ | _pending_ | _pending_ |

Only the knob-first network is held to the bar; the other column is the fallback design if live
inputs had proved unsafe. Registration: _pending_ µs per plug-in entry, _pending_ µs per port edit
on the 170-attribute node.

Reproduce, from the checkout root in natron-dev after a release build:

    build/release/Tests/Tests --gtest_filter='ShaderNode*'
    python3 tools/bench/shader_node_bench.py

If a bar is missed, the options are lazy knob creation (a knob exists only once its attribute is
edited or promoted; the rest live as a value table on the node) and a data-model design (attributes
are plain data, and knobs are created only for the node shown in the properties panel).

## Live inputs

A node's inputs are its promoted attributes, in order, and the user can change them while the node
is connected. The node's spec is built at run time and republished on every change.

`Node::initializeInputs` resizes the input array by position, so a port edit:

1. disconnects every connection whose port moves or goes away;
2. publishes the new spec;
3. calls `initializeInputs`;
4. reconnects the surviving connections by label.

Render threads may hold the old spec, so a replaced spec is retired, not freed, for the node's
life. Edits happen on the main thread only. Ports persist in a hidden `ports` knob, and are applied
before the saved connections are restored, so the loader's connect-by-label finds them. Group
Python export writes knobs before `connectInput`, so it round-trips too. One undo command restores
the old ports and the connections the edit removed. Cycles are refused on promoted ports like on
any input.

Tested: adding an input keeps existing connections; removing a connected input disconnects it
cleanly; reordering moves connections with their ports; undo; editing through the `ports` knob;
cycles; save and load.

`NodeGroup::notifyNodeDeactivated` erases one entry from a Group's inputs and then calls
`initializeInputs`, so the connections after it land under shifted labels. Port edits avoid this by
disconnecting first, and the production node should too.

## Registration

**Choice: one class with a runtime spec, registered as one plug-in entry per kind.** The entry
gives each kind its own ID, label, menu place and saved identity, and every entry builds the same
class, which reads its layout from the plug-in ID. A plug-in per kind costs one registration each
(measured above); a spec per kind costs nothing extra per node, since nodes of a kind share the
initial layout and only diverge when their ports change.

The alternative — a single plug-in whose kind is a knob — would give every node one ID, so menus,
search and saved projects could no longer tell a BSDF from a texture without reading a knob, and
changing the kind would rebuild every knob.

## Named outputs

**Choice: the downstream node records which output each input reads, in a hidden persistent
`inputOutputs` knob (`label=output` entries); an empty entry means the upstream's first output.**
Serialization, YAML and the clipboard are unchanged: the name travels with the knob through save,
load, copy and paste, and Python export.

The alternative was extending the inputs map in the node serialization from
`label → node` to `label → (node, output)`. That is the cleaner data model, and the production
node may want it, but it changes the serialization format, its YAML encoding, the paste remapping
in the GUI and the Python export for every node in Natron, to serve only port nodes.

## Port types and targets

A port has a type; a node declares targets. A connection is allowed when:

1. the two nodes share at least one target;
2. the named output exists on the upstream node;
3. the input is a promoted port;
4. the output's type converts to the port's.

Conversions are equal types, float to and from `color3` and `vector3`, `color3` to and from
`vector3`, and int to float. `closure` converts only to `closure`. The core knows types and target
names as strings; it knows nothing about shading.

One network per context:

- **Cycles shader** (target `cycles`): `ImageTexture.Color` → `PrincipledBsdf.baseColor`;
  `ImageTexture.Alpha` → `PrincipledBsdf.alpha`; `PrincipledBsdf.BSDF` (closure) →
  `MaterialOutput.surface`.
- **MoonRay shader** (target `moonray`): `ImageMap.out` (color3) → `DwaBaseMaterial.albedo`;
  `NoiseMap.out` (float) → `DwaBaseMaterial.roughness`; the material's `surface` output is the
  network's.
- **MaterialX shader** (target `materialx`): `image.out` (color3) → `standard_surface.base_color`;
  `standard_surface.out` (surfaceshader) → `surfacematerial.surfaceshader`. Nodes that exist in
  more than one renderer list several targets, and a network compiles for any target all its nodes
  share.
- **Kernel** (target `kernel`, used by geometry, particle, image and channel networks):
  `GlobalsP.P` (vector3) → `Noise.position`; `Noise.value` (float) → `Multiply.a`;
  `Multiply.out` → `OutputP.P` through float→vector3. The same node set serves geometry and
  particles because the globals node, not the core, decides what a point is.
- **Channel sets** (target `channels`): `ChannelRead.rgba` (channelset) → `Shuffle.in`;
  `Shuffle.out` → `ChannelMerge.a`. A channel set is one more port type.

Every example uses the same check, so nothing shader-specific is in the core.

## Drawing

A scratch `PortNodeGui` (not landed) draws named ports in rows: inputs on the left, outputs on the
right, with pipes curving from an output dot to an input dot. `NodeGraph::createNodeGUI` dispatches
it beside `DotGui` and `BackdropGui`.

**Decision: ports are the single connection model.** Each `NodeGui` owns a `ConnectorLayout`
(`inputAnchor(i)`, `outputAnchor(name)`, approach direction, hit test), and the one `Edge` class
takes its endpoints and tangents from it. Classic nodes get a `RadialLayout` that reproduces
today's angles, including the mask, side-input and inspector rules; port nodes get a
`PortListLayout` that lists ports down the sides. The graph items sit on Node inputs plus a port
descriptor (type, page, output name, promoted), with no new engine connection store.

Why: there is one connection class to keep correct, not two that must stay in step, and later work
(port groups, folding, channel networks) builds on the layout instead of on special cases. The
rejected alternatives were a sibling port-edge class, which would duplicate selection, dragging,
splice hints, bend points and Dot insertion that 13 other GUI files already get from `Edge`, and a
sibling item hierarchy beside `NodeGui`, which fails because `NodeGraph` assumes `NodeGuiPtr` for
selection, undo, backdrops, copy and viewer inputs.

What the prototype showed: with the scratch `PortNodeGui`, `NodeGui`'s positional input arrows
reappear on port nodes. Hiding them (making them transparent) is a hack, and it is the reason the
fix has to sit at the layout level.

Staging:

1. M350: the runtime spec carries port type, page and a `promoted` flag.
2. M351, step 1: add `ConnectorLayout` and move today's angle logic into `RadialLayout` with no
   visible change; classic graph goldens must stay pixel-identical.
3. M351, step 2: add `PortListLayout`, bezier pipes and output names, and delete the prototype's
   arrow hiding.
4. Later: port groups, folding and search-to-connect.

Not yet prototyped: splice, bend points and Dot insertion with bezier pipes.

## Look and interaction

- **Type colours** come from a type table in the core: float light grey, int lime, vector3 dark
  blue, normal/point blue-violet, color yellow, string light blue, closure green, displacement
  teal, texture apricot, channel magenta, custom white with a coloured ring.
- **Port shapes:** a circle for a single value, a pill for an array, a diamond for a varying value
  in kernel and channel networks.
- **Pipes:** bezier curves with horizontal tangents, coloured by the source port's type.
- **Dragging a pipe:** compatible ports are highlighted and incompatible ones dim. A refused drop
  says why in the status bar.
- **Groups:** collapsible port groups that mirror the knob pages.
- **Header colour:** by node category.
- **No inline value widgets.** An unconnected promoted port shows at most a read-only summary (a
  swatch or a number); editing stays in the properties panel. Inline widgets suit 5-10 ports, not
  100+, and the real `DwaBaseMaterial` has 101.
- **Default fold:** connected ports only for a node with more than 20 ports, otherwise all
  promoted ports. Header-only is a manual state.
- **Interaction:**
  - drag from any port, input or output;
  - dropping on empty space opens the node menu, filtered to nodes with a shared target and a
    type-compatible port, and the new node is auto-wired;
  - dragging a pipe's input end rewires it;
  - ctrl-drag from an output adds another pipe, since outputs fan out freely;
  - Dot nodes serve as reroutes.

## Recommendation

_Pending the measurements._ If every bar passes, build the production nodes as real Natron nodes
with these choices: one class with a runtime spec and an entry per kind, live ports with
disconnect, rebuild and reconnect by label, named outputs on the downstream node, and the
two-layer target-then-type check.

## Open questions

- Should `Node::connectInput` itself refuse connections that fail the target or type check, so
  Python and paste cannot make them, rather than leaving the check to the GUI?
- Should the 170-attribute top-end layout stay the size of record, given that the real
  `DwaBaseMaterial` has 101 attributes?
- Fix the positional input shift in `NodeGroup::notifyNodeDeactivated` before production?
