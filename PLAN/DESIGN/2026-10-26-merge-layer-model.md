# Merge layer model and panel (M101 - Clean-Sheet Merge Node, UAT round 1)

Proposal for user sign-off; nothing built.

## Model
Merge's own `output` knob becomes the node's layer knob (a pipeline policy adopting a node's own knob, new `Node::adoptLayerKnob` near `Node.cpp:3613`), so the host `layer` knob is never created. `bLayer` gains `All` (every B layer; uses `setAllowAll`, `KnobLayerSelect.h:141-151`) and `Matching` (layers on both sides), replacing the `layers` choice. "Mask all B layers by A's alpha" = `bLayer` All, `aAlphaSource` Channel, `aAlpha` rgba.A, `operation` mask. A layer missing on A reads zero while A keeps its alpha (eb8db920f); only `addProducedPlanes` must list every B layer for All. Risk: an All target resolves to no layer; `EffectInstance.cpp:4750-4770` must treat it as every produced plane.

## Knobs (script → label)
- `operation` → Operation (39 ops, over); `setBBox` → Set BBox (union/intersection/A/B, union); `metadataFrom` → Metadata From and `rangeFrom` → Range From (A/B, B)
- `bLayer` → B Layer (All, Matching, B's layers; rgba); `bAlphaSource` → B Alpha Source (Channel / Per Layer / Per Channel); `bAlpha` → B Alpha (rgba.A) or `bAlphaLayer` → B Alpha Layer, whichever the source needs
- `aLayer`, `aAlphaSource`, `aAlpha`, `aAlphaLayer`: same for A; A Layer disabled (shows All) while B is All/Matching
- `alsoMerge` → Also Merge (channel set, single-layer B only); `output` → Output (target layer, R G B A buttons, follows B Layer, shows All when B is All/Matching)
- host Mask / Invert Mask / Mix footer stays; removed: `layers`, host `layer`

## Panel
```
Operation [over v]  Set BBox [union v]  Metadata From [B v]  Range From [B v]
------------------------------------------------------------------------------
B Layer [rgba v]  B Alpha Source [Channel v]  B Alpha [rgba.A v]
A Layer [rgba v]  A Alpha Source [Channel v]  A Alpha [rgba.A v]
Also Merge [none v] (+)
Output [rgba v] [R][G][B][A]
------------------------------------------------------------------------------
[x] Mask [rgba.A v]  [ ] Invert Mask
Mix [1.0 -------o]
```

## Menu and shortcuts
Operator presets (plus Mask, Stencil) in a Merge Presets submenu (`Gui/Resources/PyPlugs/initGui.py:16-75`); Mask All Layers and Stencil All Layers in the Merge menu. Keybinds `kShortcutGroupNodes/Merge` next to M (`GuiApplicationManager.cpp:1155`), dispatched in `NodeGraph::keyPressEvent` (`NodeGraph25.cpp:388-410`) matching virtual keys (Shift+; arrives as Key_Colon). No In/Out shortcuts exist in the tree.

## Tasks
1. Own layer knob (policy + adoptLayerKnob); 2. Merge model (knobs, All/Matching, produced planes, All-target fix); 3. panel layout + screenshots; 4. callers `layer` → `output` (LightWrap.py, Glow.py, docs); 5. menu and shortcuts + `Tests/gui/m101_merge_presets.py`.

## Revision (user, 2026-10-26) — supersedes Model, Knobs and Panel above
- `layers` → Layers: multi-select list of B's layers with an All entry (the KnobChannelSet-style row list AddLayers uses, plus All). Listed layers are merged in place; all other B layers pass through untouched. Default `rgba`.
- No `output` knob, no `bLayer`, no Matching B mode, no `alsoMerge`, no new-layer creation. The host `layer` knob is not created on Merge.
- `aLayer` → A Layer: Matching (default; each listed layer reads A's same-named layer, a missing one reads zero while A's alpha still applies) or one of A's layers, merged into every listed layer.
- `bAlphaSource`/`bAlpha`/`bAlphaLayer`, `aAlphaSource`/`aAlpha`/`aAlphaLayer` as above.

```
Operation [stencil v]  Set BBox [union v]  Metadata From [B v]  Range From [B v]
------------------------------------------------------------------------------
Layers   [spec] [diffuse] [albedo] (+)      or  [All]
B Alpha Source [Channel v]  B Alpha [rgba.A v]
A Layer  [Matching v]  A Alpha Source [Channel v]  A Alpha [rgba.A v]
------------------------------------------------------------------------------
[x] Mask [rgba.A v]  [ ] Invert Mask
Mix [1.0 -------o]
```
Example: stencil spec, diffuse and albedo by A's alpha = Operation stencil, Layers spec/diffuse/albedo, A Alpha rgba.A.
