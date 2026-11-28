# Port nodes: shader, kernel and channel networks

Real Natron nodes as network nodes: one generic port-node class and one container class serve
shaders, kernel networks and channel networks. Each node declares the targets it supports and its
typed ports; wiring checks targets first, then port types. The evidence here comes from a test
effect, `PortTestNode` (`Engine/Nodes/Test/`), the `ShaderNode*_Test` gtests and
`tools/bench/shader_node_bench.py`.

## Measurements

Network: 50 nodes in a Group — 5 large materials (101 attributes each, the real
`DwaBaseMaterial` count), 10 BSDFs (35 attributes), 5 two-output image textures and 30 small maps
— with 40 edges, about 10 ports per node. The 30 maps feed the materials (MoonRay), and each
texture feeds two BSDFs, one from each output (Cycles), so no edge crosses targets. Knob edits
re-author the whole network as UsdShade shaders (every attribute value and connection) on an
anonymous layer. Comp edits are a 20-Grade chain: a knob change plus a reconnection per round,
timed with and without the network in alternating pairs after both are warmed, as medians; the
graph_bench re-render is the median over several sessions per network in rotating order. A
170-attribute top-end material layout exists too, but only the port-edit cost below is measured
on it.

| Bar | Limit | Knob-first | Every attribute a port | Result |
|---|---|---|---|---|
| Open | < 2 s | 128.0 ms | 118.0 ms | pass |
| Save | < 2 s | 14.9 ms | 17.0 ms | pass |
| Knob edit re-authors | < 100 ms | 29.4 ms | 30.6 ms | pass |
| Comp edits (in process) | no slower (within 10 %) | to re-measure | to re-measure | to re-measure |
| Comp edits (graph_bench re-render) | no slower (within 10 %) | to re-measure | to re-measure | to re-measure |

Only the knob-first network is held to the bar; the other column is the fallback design if live
inputs had proved unsafe. Registration: 1.6 µs per plug-in entry (over 200 entries; the port
nodes register 10), 86.4 µs per port edit on the 170-attribute material.

Network sizes: knob-first has 245 inputs and 1865 knobs (create: to re-measure);
every-attribute-a-port has 1015 inputs and the same 1865 knobs (create: to re-measure). Both have
50 nodes and 40 edges. Create is timed on the last of several builds, after the comp-edit pairs.

Caveats:

- The worst single knob edit on the knob-first network was 541.1 ms (every-attribute-a-port: 32.2
  ms), which the bench prints against the bar without gating it. The bar is on the typical edit,
  which passed at 29.4 ms, but the outlier is unexplained and worth a follow-up investigation, for
  example a first-edit cache warm-up.
- The bench ran in a release build in the natron-dev container on the dev box (an N100-class
  machine), on 2026-11-22. A faster machine should do better, but the numbers are not a survey of
  hardware.

Reproduce, from the checkout root in natron-dev after a release build:

    build/release/Tests/Tests --gtest_filter='ShaderNode*'
    python3 tools/bench/shader_node_bench.py

If a bar is missed, the options are lazy knob creation (a knob exists only once its attribute is
edited or shown as a port; the rest live as a value table on the node) and a data-model design (attributes
are plain data, and knobs are created only for the node shown in the properties panel).

## Live inputs

A node's inputs are its shown attributes, in order, and the user can change them while the node
is connected. The node's spec is built at run time and republished on every change.

`Node::initializeInputs` resizes the input array by position, so a port edit:

1. disconnects every connection whose port moves or goes away;
2. publishes the new spec;
3. calls `initializeInputs`;
4. reconnects the surviving connections by label.

Render threads may hold the old spec, so a replaced spec is retired, not freed, for the node's
life. Edits happen on the main thread only. Ports persist in a hidden `ports` knob, and are applied
before the saved connections are restored, so the loader's connect-by-label finds them. Group
Python export writes knobs before `connectInput`; running the exported script into an empty Group
rebuilds the ports, connections and output names. One undo command restores the old ports, the
connections the edit removed and their output names. Cycles are refused on shown ports like on
any input.

Tested: adding an input keeps existing connections; removing a connected input disconnects it
cleanly; reordering moves connections with their ports; undo; editing through the `ports` knob;
cycles; save and load; a Group's Python export run into a new Group.

`NodeGroup::notifyNodeDeactivated` erases one entry from a Group's inputs and then calls
`initializeInputs`, so the connections after it land under shifted labels. Port edits avoid this by
disconnecting first, and the production node should too.

## Registration

**Choice: one class with a runtime spec, registered as one plug-in entry per kind.** The entry
gives each kind its own ID, label, menu place and saved identity, and every entry builds the same
class, which reads its layout from the plug-in ID. A plug-in per kind costs one registration each
(measured above). Nodes of a kind share the kind's layout, but not a spec: each node builds its
own spec from its ports when it is created and a new one on every port edit, and keeps every spec
it has published for its life, since render threads may still read an old one. A spec is the
input labels and descriptors, small beside the node's knobs.

The alternative — a single plug-in whose kind is a knob — would give every node one ID, so menus,
search and saved projects could no longer tell a BSDF from a texture without reading a knob, and
changing the kind would rebuild every knob.

## Named outputs

**Choice: the downstream node records which output each input reads, in a hidden persistent
`inputOutputs` knob (`label=output` entries); an empty entry means the upstream's first output.**
Serialization, YAML and the clipboard are unchanged: the name travels with the knob through save,
load, copy and paste, and Python export.

Lifetime: an entry lasts while its port exists and its input stays with the same upstream node.
Removing the port drops it, and so does connecting the input to a different node than it was last
connected to, or to a node without that output; the input then reads the new node's first output.
A disconnected input keeps its entry, so that undoing the upstream node's deletion reconnects it
to the same output. Loading, pasting and running an exported script set the names before
connecting, with no previous upstream, so the names survive. The `ports` knob must be set before
`inputOutputs`: setting the ports drops the names of ports the node does not have.

The alternative was extending the inputs map in the node serialization from
`label → node` to `label → (node, output)`. That is the cleaner data model, and the production
node may want it, but it changes the serialization format, its YAML encoding, the paste remapping
in the GUI and the Python export for every node in Natron, to serve only port nodes.

## Port types and targets

A port has a type; a node declares targets. A connection is allowed when:

1. the two nodes share at least one target;
2. the named output exists on the upstream node;
3. the input is a shown port;
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

Port nodes draw named ports in rows: inputs on the left, outputs on the right, with pipes curving
from an output to an input.

**Decision: ports are the single connection model.** Each `NodeGui` owns a `ConnectorLayout`
(`inputAnchor(i)`, `outputAnchor(name)`, approach direction, hit test), and the one `Edge` class
takes its endpoints and tangents from it. Classic nodes get a `RadialLayout` that reproduces
today's angles, including the mask, side-input and inspector rules; port nodes get a
`PortListLayout` that lists ports down the sides. The graph items sit on Node inputs plus a port
descriptor (type, page, output name, shown), with no new engine connection store.

Why: there is one connection class to keep correct, not two that must stay in step, and later work
(port groups, folding, channel networks) builds on the layout instead of on special cases. The
rejected alternatives were a sibling port-edge class, which would duplicate selection, dragging,
splice hints, bend points and Dot insertion that 13 other GUI files already get from `Edge`, and a
sibling item hierarchy beside `NodeGui`, which fails because `NodeGraph` assumes `NodeGuiPtr` for
selection, undo, backdrops, copy and viewer inputs. Drawing ports on a `NodeGui` that still places
inputs by angle leaves its positional input arrows on port nodes, and hiding them is a hack; the
layout is where that choice belongs.

Build order:

- The runtime spec carries each port's type, page and a `shown` flag.
- `ConnectorLayout` arrives with today's angle logic moved into `RadialLayout` and no visible
  change: classic graph goldens stay pixel-identical.
- `PortListLayout`, bezier pipes and output names follow on that base.
- Port groups, folding and shift-drop node creation come after. Shift-drop with port cycling then
  comes to the classic graph and to plain Tab creation too.

Still to work out: splice, bend points and Dot insertion with bezier pipes.

## Look and interaction

- **Type colours** come from a type table in the core: float light grey, int lime, vector3 dark
  blue, normal/point blue-violet, color yellow, string light blue, closure green, displacement
  teal, texture apricot, channel magenta, custom white ring.
- **Port shapes:** a circle for a single value, a pill for an array, a diamond for a varying value
  in kernel and channel networks.
- **Pipes:** bezier curves with horizontal tangents, coloured by the source port's type.
- **Dragging a pipe:** compatible ports are highlighted and incompatible ones dim. A refused drop
  says why in the status bar.
- **Groups:** collapsible port groups that mirror the knob pages.
- **Header colour:** by node category.
- **No inline value widgets.** An unconnected shown port shows at most a read-only summary (a
  swatch or a number); editing stays in the properties panel. Inline widgets suit 5-10 ports, not
  100+, and the real `DwaBaseMaterial` has 101.
- **Default fold:** connected ports only for a node with more than 20 ports, otherwise all
  shown ports. Header-only is a manual state.
- **Interaction:**
  - clicking an output port picks up a new pipe from it, so outputs fan out freely;
  - clicking an input port picks up its pipe, or a new one when it is unconnected;
  - clicking a pipe picks it up by the end nearer the click, input or output, as edges do on
    today's graph;
  - dropping a pipe on a port connects it;
  - dropping a pipe on empty space disconnects it;
  - shift-dropping a pipe on empty space opens the Tab node menu, filtered to nodes with a shared
    target and a type-compatible port. The new node's compatible ports are highlighted, inputs or
    outputs depending on which end of the pipe is being placed. The first one is picked, and the
    scroll wheel or the up and down keys move the pipe to the next or previous compatible port;
  - Dot nodes serve as reroutes.

## Recommendation

**Verdict: PASS on open, save and knob edits; comp edits to re-measure.** The knob-first network
meets the open and save bars with wide margin (about 15x and 130x under the limit) and is about 3x
under the knob-edit limit. The comp-edit rows are to re-measure; if they pass, the lazy knob
creation and data-model fallbacks above are not needed.

Build the production nodes as real Natron nodes with these choices: one class with a runtime spec
and an entry per kind, live ports with disconnect, rebuild and reconnect by label, named outputs
on the downstream node, and the two-layer target-then-type check. For the graph, follow the
decisions under Drawing: ports are the single connection model through a per-`NodeGui`
`ConnectorLayout` (`RadialLayout` first, pixel-identical, then `PortListLayout`); unconnected
shown ports show read-only summaries, with no inline value widgets; a node with more than 20
ports shows connected ports only by default; varying values use diamond ports.

Before production, investigate the 541 ms worst knob edit.

## Open questions

- Should `Node::connectInput` itself refuse connections that fail the target or type check, so
  Python and paste cannot make them, rather than leaving the check to the GUI?
- Should the 170-attribute top-end layout stay the size of record, given that the real
  `DwaBaseMaterial` has 101 attributes?
- Fix the positional input shift in `NodeGroup::notifyNodeDeactivated` before production?
