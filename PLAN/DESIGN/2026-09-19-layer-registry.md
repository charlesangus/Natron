# Design: the project-level layer registry (M38, Phase 38.1)

2026-09-19. Foundational design for approval; precedes the three-knob widget work in `2026-09-19-layer-channel-widget.md` (whose engine/render citations are carried over unchanged). Governing user direction: a global layer cannot live on a node; Natron needs script-level knowledge of layers before any layer knob can offer "New layer…" or target a layer that is not yet upstream.

## 0. What exists today (the facts the design builds on)

**`ImageLayerDesc`** (`Engine/ImageLayerDesc.h:75-255`) is `{_layerID, _layerLabel, _channels, _channelsLabel}`. ID is the identity (`getLayerID()` `:110`, "not for display"); label is display (`:116`); `_channelsLabel` is a type hint ("Motion", "XY", else the concatenation of channel names, `.cpp:66-71`). **Equality is ID + channel *count* only** — not channel names, not label (`ImageLayerDesc.cpp:132-139`); `operator<` is ID only (`:141-145`). `findEquivalentLayer` (`.h:200-226`) treats *any* Color variant (RGBA/RGB/Alpha/XY all carry ID `kFnOfxImagePlaneColour`, `.cpp:184-214`) as matching Color, otherwise `operator==`. Built-ins: Color, Backward/Forward motion (U,V), DisparityLeft/Right (X,Y) with OFX-standard IDs (`.h:55-71`). The OFX plane string is `NatronOfxImageComponentsPlaneName_<id>[_PlaneLabel_<label>][_ChannelsLabel_<cl>]_Channel_<c1>_Channel_<c2>…` (`libs/OpenFX/include/ofxNatron.h:100-189`), encoded/decoded at `ImageLayerDesc.cpp:299-394, 450-470`; **the decoder refuses more than 4 channels** (`:382-385`). Boost serialization of the four fields is at `Engine/ImageParamsSerialization.h:77-95`.

**Where layers enter a script today.**
- *Read*: ReadOIIO parses channel names into layers (`build/assets/plugin-src/openfx-io/OIIO/ReadOIIO.cpp:1002-1245`; bare R/G/B/A/I/Y → `"Color"` `:1140-1141`; lone `Z` → layer `"depth"` `:326, :1165-1172`; `X,Y,Z` together → XYZ), caps a layer at 4 channels (`:364`), and reports every non-Color plane on its output clip from `getClipComponents` as `ImagePlaneDesc(name, "", "", channelNames)` (`:769-816`). The host turns those strings into `ImageLayerDesc` in `OfxEffectInstance::getComponentsNeededAndProduced` (`Engine/OfxEffectInstance.cpp:2877-2917`). So a file layer arrives as ID = label = file layer name, channels as in the file.
- *"New…" on the host-made Output Layer choice*: `Node.cpp:2947` `setHostCanAddOptions(isOutput)` → `Gui/KnobGuiChoice.cpp:305-306` adds the entry → `onItemNewSelected` (`:312-340`) opens `NewLayerDialog` and calls `Node::addUserComponents` (`Engine/Node.cpp:7714-7755`), which stores into `NodePrivate::createdComponents` (`Engine/NodePrivate.h:455-456`) and is serialized per node as `UserComponents` (`Engine/NodeSerialization.h:267, 321`; restored at `Node.cpp:1481-1484`). No shipped plugin declares `kNatronOfxParamPropChoiceHostCanAddOptions` (grep of openfx-misc/openfx-io: zero hits); Shuffle only picks among present planes (`Shuffle.cpp:607-609, 1917`).
- *Project defaults*: **a project "Layers" page already exists** — `KnobLayers defaultLayersList` named `defaultLayers` (`Engine/Project.cpp:1087-1122`), seeded with the five built-ins; `Project::getProjectDefaultLayers()` (`:1478-1530`) re-parses the knob's table string on every call and maps labels back to IDs by a hand-written switch (`:1494-1507`); `addProjectDefaultLayer` (`:1533-1549`) appends without any uniqueness check; Python `App.addProjectLayer` (`Engine/PyAppInstance.cpp:497-500`). The change handler only refreshes node menus when `reason == eValueChangedReasonUserEdited` (`:1776-1781`) — `KnobTable::setTable` uses `setValue` with `eValueChangedReasonNatronInternalEdited` (`Engine/KnobTypes.cpp:2511-2514`, `Engine/KnobImpl.h:1798-1808`), so **the Python path never refreshes any dropdown**. The table encoding tags cells with the *translated* column label (`KnobTypes.cpp:2478-2494`, labels from `tr()` at `KnobTypes.h:1174-1182`) — locale-dependent on disk — and the project writes a third column for which `getColumnLabel(2)` is `""`, producing `<></>` junk that the decoder skips.
- *Availability*: `EffectInstance::getAvailableLayers` (`Engine/EffectInstance.cpp:4453-4519`) = produced ∪ pass-through, Color hoisted first, then (only for `inputNb == -1`) project defaults (`:4496-4509`), then **this node's** user components (`:4513-4517`, note `getNode()` not `effect` — a node's own user layers are merged into its *input* lists too). The same pair is merged again in `getComponentsNeededDefault` (`:4288-4297`) so a selected-but-absent layer becomes a *produced* plane (the implicit shuffle M38 removes). `getUserLayers()` (`:182-196`) publishes project defaults ∪ node user layers to plugins as `kNatronOfxExtraCreatedPlanes` (`ofxNatron.h:230-238`, `Engine/OfxImageEffectInstance.cpp:288-311`); no shipped plugin reads it. `kFnOfxImageEffectPropComponentsPresent` on a clip is computed on demand from `getAvailableLayers` (`Engine/OfxClipInstance.cpp:293-315`).
- *Refresh*: input change → `forceRefreshAllInputRelatedData` (`Engine/NodeInputs.cpp:1588-1591`) → `Node::refreshAllInputRelatedData` (`Node.cpp:6570-6630`: `refreshMetadata_public` `:6611`, then `refreshChannelSelectors()` `:6614`); metadata change downstream via `EffectInstance::refreshMetadata_recursive` (`EffectInstance.cpp:5599-5628`, selectors at `:5619`). `refreshChannelSelectors` (`Node.cpp:7639-7711`) rebuilds each choice from `getAvailableLayers` and ends with `onChannelsSelectorRefreshed()` (`:7708`), which the viewer turns into `availableComponentsChanged` (`Engine/ViewerInstance.cpp:3358-3362` → `Gui/ViewerTab.cpp:1034`). The viewer's list is `activeInput->getAvailableLayers(time, 0, -1)` (`Gui/ViewerTabPrivate.cpp:383-388`), so it currently lists project defaults that are not present.
- *Render/cache*: `getComponentsNeededAndProduced_public` results are cached per (node hash, time, view) (`Engine/EffectInstancePrivate.h:67-161`); `Node::computeHash` (`Node.cpp:786-888`) hashes knobs age, inputs, script name, project creation time — **nothing about available layers**. `ImageKey` carries no plane (`Engine/ImageKey.cpp:68-79`); the plane is in `ImageParams::_components` and compared on lookup (`Engine/ImageParams.h:246, 262`). Render-time plane selection: `EffectInstanceRenderRoI.cpp:478-531`.
- *Project*: still boost XML (`Project.cpp:746` save, `:441` load; the zip bundle of `docs/decisions/2026-09-05-project-format-bundle-design.md` is unimplemented). Restore order in `ProjectPrivate::restoreFromSerialization` (`Engine/ProjectPrivate.cpp:103-260`): formats `:118-141` → **project knobs `:148-196`** → nodes `:205-206` → links `:217-230` → `forceComputeInputDependentDataOnAllTrees()` `:236`, all under the `isLoadingProject` flag (`Project.cpp:401`). Schema: `ProjectSerialization{_nodes, _additionalFormats, _projectKnobs, …}` (`Engine/ProjectSerialization.h:99-104`, save order `:207-217`, version `:74`). Formats are the analogue: `additionalFormats` list + `formatMutex` (`ProjectPrivate.h:69-77`), `setOrAddProjectFormat` (`Project.cpp:2275-2318`) pushes `refreshFormatParamChoice` to every node (`Node.cpp:3051-3075`); signals `mustCreateFormat`/`formatChanged`/`projectViewsChanged` (`Project.h:380-392`, consumer pattern `Engine/OneViewNode.cpp:53, 166-178`).
- *Deep*: `DeepRead` reads flat channel-name strings (`Engine/Nodes/Deep/DeepRead.cpp:214-284`); `DeepImage` stores `map<string, DeepChannelBuffer>` (`Engine/DeepImage.h:301`); no layer grouping.
- *Python/PyPlugs*: `Effect.addUserLayer` (`Engine/PyNode.cpp:991-1009`), `App.addProjectLayer` (above), `ImageLayer` value type (`Engine/PyNode.h:47-100`, `Engine/typesystem_engine.xml:715`); no `Project` wrapper exists (`Engine/PyAppInstance.h:306` exposes only `getProjectParam`). No shipped PyPlug calls `addUserLayer`/`addProjectLayer` (grep of `Gui/Resources/PyPlugs`: zero hits); the PyPlug exporter emits `lastNode.addUserLayer(...)` per node (`Engine/NodeGroup.cpp:2733-2745`).

**The Nuke model** (from the Nuke User Guide "Channels" chapter and the NDK `DD::Image::Channel`/`ChannelSet` reference; concepts, not URLs): channels are global to the script, at most 1023 uniquely named; a *layer* is a named group of channels addressed as `layer.channel`; channels come into existence from Read nodes (EXR channel names, with `R,G,B,A`→`rgba`, `Z`→`depth.Z`), from the "new" entry of any channel knob (a dialog asking for a layer name and channel names), or from Python `nuke.Layer(name, [channels])`; every layer is written at the top of the `.nk` as `add_layer {name name.ch1 name.ch2 …}`; a channel, once created, stays in the script's lists even if no node uses it and there is no UI to delete it; every channel knob (Copy, Shuffle, Roto's *output*, Merge's A/B/output) lists the whole registry, so a node can target a layer that no upstream node carries — a Roto painting into `mask.a` *creates* that channel in its output stream; `ChannelSet` is the set type with the named sets `rgb`, `rgba`, `alpha`, `all`, `none`; the channel knob UI is a 4-column grid per layer, but a layer may hold more than four channels (they appear under the layer's sub-menu), and the 2019 Shuffle addresses any channel by name.

## 1. The registry

### 1.1 Shape

A new value class `LayerRegistry` (`Engine/LayerRegistry.h/.cpp`), owned by `ProjectPrivate` — replacing the `defaultLayersList` knob as the source of truth:

```cpp
struct LayerRegistryEntry {
    ImageLayerDesc desc;
    enum Origin { eOriginBuiltin, eOriginUser, eOriginFile, eOriginPlugin } origin;
};
class LayerRegistry {
    // main-thread mutation, any-thread read (see §5)
    bool add(const ImageLayerDesc&, Origin, std::string* error);        // false + reason on conflict
    bool remove(const std::string& id, std::string* error);             // refused for built-ins
    bool contains(const std::string& id) const;
    bool find(const std::string& id, ImageLayerDesc* out) const;
    std::shared_ptr<const std::vector<LayerRegistryEntry>> snapshot() const;   // ordered
    static bool validate(const ImageLayerDesc&, bool fromFile, std::string* error);
    static const ImageLayerDesc* reservedAlias(const std::string& id);  // "rgba"→Color etc.
};
```

**Order**: the five built-ins first, in the order `Project.cpp:1096-1101` already uses (Color, DisparityLeft, DisparityRight, Backward, Forward), then everything else in registration order. Dropdowns show registry order; no alphabetical sorting (a stable order is what users learn; Nuke's channel menus are also insertion-ordered within a layer).

**Identity**: unique by **ID**, compared case-sensitively (EXR channel names are case-sensitive and `Diffuse`/`diffuse` can coexist in one file). The registry does not use `ImageLayerDesc::operator==` (ID + count) for identity — it uses the ID alone — because the same file layer can legitimately change channel count (see 1.3).

**Label**: for non-built-in layers, label == ID, always; there is no rename in M38 (rename = remove + add). Built-ins keep their display labels (`Color`, `Backward`, …). Consequence: label uniqueness is ID uniqueness plus the reserved list below. Regex rows (which match labels) therefore match IDs for every user/file layer — what users see is what they type.

**Reserved names** (rejected or aliased case-insensitively, since these are what users will type by habit):
- `Color`, `rgba`, `rgb`, `alpha`, `RGBA`, `RGB`, `A`, `XY` → `add()` returns the built-in Color layer and registers nothing (the ID `kFnOfxImagePlaneColour` is the only Color).
- `none`, `all` → rejected (they are widget modes).
- The built-in labels (`Backward`, `Forward`, `DisparityLeft`, `DisparityRight`, `Motion`, `Disparity`) → rejected (they would be indistinguishable in a dropdown).
- `depth` is **not** reserved: it is pre-registered (below).

**Channel-name validation** (`validate`): 1 to **4** channels; each matches `[A-Za-z0-9_]+` (no dots — the dot is the `layer.channel` join, `ImageLayerDesc.cpp:248-270`; no whitespace — the current table encoding splits on spaces, `Project.cpp:1519-1520`); unique within the layer. Layer ID: non-empty, no whitespace, no leading/trailing dot; dots *inside* an ID are allowed only when `fromFile` (nested EXR layers such as `light1.diffuse` reach the host that way and must round-trip; the dialog forbids them). **Cap at 4**, not N: the render pipeline is `std::bitset<4>` end to end (`Engine/EffectInstance.h:1091, 1990`, `Engine/Image.h:843-968`), the OFX plane decoder refuses >4 (`ImageLayerDesc.cpp:382-385`), ReadOIIO refuses >4 (`ReadOIIO.cpp:364`) and `Effect.addUserLayer` refuses >4 (`PyNode.cpp:994`). Lifting the cap is an image-storage change, not a registry change; the registry stores a vector and the cap is a single constant `kLayerMaxChannels` in `validate`, so the widget's "N buttons for an N-channel layer" already works for N ∈ 1..4 and needs no rewrite when the cap moves.

**Pre-registered defaults**: the five built-ins (immutable, `eOriginBuiltin`) plus one removable `eOriginUser` entry `depth` `[Z]`, because ReadOIIO already names a lone `Z` channel `depth` (`ReadOIIO.cpp:326, 1165-1172`) and Nuke's `depth.Z` is the convention every EXR pipeline expects. Nothing else.

### 1.2 Serialization and load order

`ProjectSerialization` gains `std::list<LayerRegistryEntry> _layers` written as `Layers` (each entry: the existing `ImageLayerDesc` NVPs `LayerID/LayerLabel/ChannelsLabel/Channels`, `ImageParamsSerialization.h:79-83`, plus `Origin`), saved after `AdditionalFormats` (`ProjectSerialization.h:215`); `PROJECT_SERIALIZATION_VERSION` 6 → 7 (`:74`). Built-ins are **not** written (they are code); every other entry is, including file-discovered ones (Nuke behaviour: the script remembers its channels even when the file is gone).

Restore: in `ProjectPrivate::restoreFromSerialization`, right after formats (`ProjectPrivate.cpp:141`) and before project knobs — i.e. **before any node exists** (`:205`), the same guarantee the format registry relies on for `Node::findPluginFormatKnobs` (`Node.cpp:2168-2212`). Nodes reference layers by ID string in their knobs; at node-load time every saved ID is already present.

**A node referencing an unregistered ID on load** (hand-edited project, or a layer removed via Python then the project saved): the knob keeps the ID string; the registry is *not* auto-populated from it (a channel-set row stores only the *enabled* channel names, not the layer's full channel list, so any auto-registration would be a guess); `resolve()` treats the row as absent and the widget shows the row with a "not in project" marker (widget phase). No error dialog on load. Decision: silent-but-visible, never invent a layer.

**Old per-node `_userComponents`**: **dropped, not migrated.** `NodeSerialization` loses the field (`NodeSerialization.h:267, 321, 430`), `NODE_SERIALIZATION_CURRENT_VERSION` bumps, `Node::loadKnobs` loses `:1481-1484`. Rationale: `docs/decisions/2026-09-18-clean-break-no-project-compat.md` says exactly this ("a serialization change needs only a version bump"); the data is a name and up to four channel names, recreated in five seconds through the dialog; and the alternative keeps `createdComponents` alive on `Node` for one load-time transfer, which is the per-node ownership the user has ruled out.

### 1.3 Conflicts

`add()` with an ID already registered:
- identical channel list → no-op, returns true (idempotent; this is the common path when a Read is re-evaluated).
- **different channels, `fromFile`** → *union*, preserving the existing order and appending new names, provided the result is ≤ 4; the entry is updated in place and nodes referencing the ID get `incrementKnobsAge()` (see §4). This is Nuke's behaviour (a second EXR with `diffuse.A` grows the `diffuse` layer) and the only choice that does not silently drop data. Beyond 4 → the add is refused with a warning in the log; the stream still carries its own descriptor, and `findEquivalentLayer` (count-sensitive) will not match it against the registry entry — the knob phase resolves by ID first and intersects channel names, so the mismatch degrades to "channels the registry does not know are not selectable", not to a render failure.
- different channels, user-typed → refused with "A layer named X already exists with channels …".

Two Color variants never conflict: `isColorLayer()` collapses them (`ImageLayerDesc.cpp:120-130`), and the registry holds one Color entry (RGBA) whose *present* variant is whatever the stream carries (`mapNCompsToColorLayer`, `:282-297`).

## 2. Who adds layers, and when

**(a) Explicit user action.** The "New…" entry of the existing host Output Layer choice (`KnobGuiChoice.cpp:312-340`) and the project settings Layers page both call `Project::addLayer(desc, eOriginUser)`; on success the choice selects the new ID (as `Node::addUserComponents` does at `Node.cpp:7746-7751`). In the widget phase the three knob types reuse the same dialog and the same call.

**(b) Automatic registration of produced layers — the generic rule: a node that *produces* a layer registers it.** Hook: the end of `Node::refreshAllInputRelatedData` (`Node.cpp:6570-6630`, main thread, already the point where the node's produced planes are known), new `Node::registerProducedLayers()`: take `comps[-1]` of `getComponentsNeededAndProduced_public` (produced only — not pass-through, not the registry merge), and for every non-Color layer not yet registered call `Project::addLayer(desc, eOriginFile if isReader() else eOriginPlugin)`. This covers Read/ReadOIIO, any OFX generator that emits a custom plane, and future native readers, with no reader-specific code. It runs at node creation, on file change (metadata refresh → `refreshMetadata_recursive` `EffectInstance.cpp:5599-5628`), and once per project load (`ProjectPrivate.cpp:236`). Not on render threads: render-side calls of `getComponentsNeededAndProduced_public` never mutate the registry. **When the file changes to one without the layer, the layer stays registered** (Nuke behaviour; nodes downstream still reference it and the stream simply no longer carries it, which `resolve()` handles as "listed but absent", §3). DeepRead is wired in M60 (its channel names are only known inside its render, `DeepRead.cpp:214-284`); 38.1 ships the shared grouping helper it will call (§7).

**(c) Python**: `app.addProjectLayer(...)`, §6.

**(d) Plugins creating output planes**: covered by (b). Shuffle's output "New" is the host "New…" entry of (a) once Shuffle is native (M34); the OFX Shuffle has no such entry today.

**Removal**: allowed only for non-built-in layers **with no references**; refused otherwise with a dialog naming the referencing nodes. "Reference" = any layer/channel-set/layer-select/channel-select knob row carrying the ID (widget phase), and in 38.1 the existing `ChannelSelector`/mask choice knobs (`NodePrivate.h:48-55`, `_imp->channelsSelectors`, `maskSelectors`). Viewer selections are not references (the viewer lists present-only, §3, and its `ViewerData::layerName` in `Gui/ProjectGuiSerialization.h:119` degrades to Color when absent). Removal is not undoable in 38.1 (nor is "New format"). Why not Nuke's "never remove": a long-lived script accumulates every layer of every EXR it ever touched; refusing removal only while referenced keeps the safety Nuke's rule buys and gives the user a way out. The Layers page also gets a "Remove unused" button (removes every `eOriginFile`/`eOriginPlugin`/`eOriginUser` entry with zero references) — the one bulk operation bloat needs.

## 3. Listed vs present

**Amended 2026-09-19 after user review — no greyed-out entries.** The listing source is a
property of the knob's *role*, not a global rule:

- **Input-bound knobs list present layers only**: a processing node's channel set, mask
  channel selects, the future Merge A/B selects. The list is exactly
  `getPresentLayers(inputNb)` (produced ∪ pass-through of the source stream); registry
  layers the stream does not carry are **not shown**, and there is no "New layer…" entry
  (a node that cannot write into a layer has no business creating one).
- **Target knobs list the registry**: Roto/RotoPaint's output layer select, generators,
  the native Shuffle's output, the future Merge output. Registry order, built-ins first,
  plus "New layer…". A writing node declaring a registry layer in `comps[-1]` **creates it
  in its output stream** (what `EffectInstanceRenderRoI.cpp:492-499` already does for a
  produced plane).

A selected layer that later disappears from an input keeps its ID in the knob's value and
contributes nothing to `resolve()`; the widget marks the *selected value* ("(not in
input)"), never the list. Which knobs are input-bound and which are targets is fixed per
knob type in the widget design; the registry does not care.

Two engine queries, both on `EffectInstance`:
- `getAvailableLayers(time, view, inputNb, out)` keeps its name and becomes **present ∪
  registry** for `inputNb == -1` only (the `getProjectDefaultLayers() ∪
  getUserCreatedComponents()` pair at `EffectInstance.cpp:4496-4517` replaced by one
  registry snapshot) — the existing choice selectors and `kNatronOfxExtraCreatedPlanes`
  keep working through 38.1; the widget phase stops using it for input-bound knobs.
- new `getPresentLayers(time, view, inputNb, out)` = produced ∪ pass-through only
  (`:4466-4493, 4511`). The viewer switches to it (`ViewerTabPrivate.cpp:385`): **the
  viewer shows what is there**, and so does every input-bound knob.

## 4. Relationship to the OFX plane model and the caches

- Plugins learn an input's planes from `kFnOfxImageEffectPropComponentsPresent`, computed on demand from `getAvailableLayers` (`OfxClipInstance.cpp:293-315`) — never cached host-side, so no plugin can hold a stale host list. For input clips (`inputNb ≥ 0`) that is present-only already (`EffectInstance.cpp:4496` gates the registry merge on `-1`); the user-components merge at `:4513-4517` goes away with `createdComponents`.
- The registry is what the host reports as the planes a node *may* produce: `getUserLayers()` (`EffectInstance.cpp:182-196`) becomes a straight registry snapshot, published as `kNatronOfxExtraCreatedPlanes` (`ofxNatron.h:230-238`) — the property already has the right meaning ("planes created by the user through some interface which should be made available in output"). It is per-project instead of per-node, which is the point.
- **A writer's "All Layers" enumerates the stream, not the registry — confirmed**: WriteOIIO's `getClipComponents` asks `_inputClip->getPlanesPresent()` (`build/assets/plugin-src/openfx-io/OIIO/WriteOIIO.cpp:573-593`) and that is the input clip's present list. Nothing in this design changes it.
- **Cache keys**: `ImageKey` has no plane (`ImageKey.cpp:68-79`); a cached image is matched by `ImageParams::_components` (`ImageParams.h:246`), which compares ID + channel count. Adding or removing a registry entry changes no node's hash and no image's params — correct, because it changes no pixel. The one registry mutation that can change pixels is the file-driven channel union (§1.3), which changes a layer's count; a plane rendered under the old count already mismatches by params, and to also flush action caches the union calls `incrementKnobsAge()` on every node that references the ID (the same mechanism a format change uses, `Project.cpp:1798-1802`). **No `NATRON_CACHE_VERSION` bump in 38.1** (no cached struct changes); the widget phase bumps 5 → 6 as planned.

## 5. Refresh, propagation, threading

- **Registry change → dropdowns**: `Project` emits a new Qt signal `projectLayersChanged()` (next to `projectViewsChanged`, `Project.h:392`) after every add/remove/union *outside* project load; `Project` itself then calls `refreshChannelSelectors()` on every node (`Node.cpp:7639-7711`, cheap: main thread, hits the actions cache) — not `forceComputeInputDependentDataOnAllTrees()`, which re-runs metadata for the whole project and is what `Project.cpp:1777-1779` does today. The widget's `KnobGui` connects to the same signal to repopulate. During load the signal is suppressed (`isLoadingProject`, `Project.cpp:401`) and emitted once after `ProjectPrivate.cpp:236`.
- **Stream change → present set**: unchanged machinery (`NodeInputs.cpp:1588-1591`, `EffectInstance.cpp:5619`, `Node.cpp:6614`); `resolve()` sees present-ness at render time through `getComponentsNeededAndProduced_public`, cached per hash.
- **Threading model**: mutations are main-thread only (asserted); readers on any thread call `snapshot()`, which returns a `shared_ptr<const vector>` swapped under a `QMutex` — readers never hold the lock while iterating, and a snapshot taken by a render thread stays valid for the whole render even if the main thread adds a layer meanwhile. This replaces today's per-call `KnobTable::getTable` re-parse of a QString on render-adjacent paths (`Project.cpp:1483`, reached from `EffectInstance.cpp:4498`).

## 6. Python API and GUI

**Python** (on `App`, because no `Project` wrapper exists — `Engine/PyAppInstance.h:306` — and `getViewNames`/`addProjectLayer` already live there):
```python
app.getProjectLayers()                 # [ImageLayer] in registry order, built-ins first
app.getProjectLayer("diffuse")         # ImageLayer or None
app.addProjectLayer("diffuse", ["R","G","B"])   # -> ImageLayer; ValueError on conflict/invalid
app.addProjectLayer(ImageLayer(...))   # existing overload kept
app.removeProjectLayer("diffuse")      # -> bool; False when built-in or referenced
```
`Effect.addUserLayer` (`PyNode.cpp:991-1009`) is **deleted** (clean break; no shipped PyPlug uses it). The PyPlug exporter (`NodeGroup.cpp:2733-2745`) stops emitting it; in the widget phase it emits `app.addProjectLayer(...)` for every non-built-in layer referenced by any node inside the group, so a PyPlug carries the layers it needs.

**GUI**: the existing project "Layers" page is kept but the knob becomes a **non-persistent view** of the registry: `KnobLayers` (`Engine/KnobTypes.h:1142-1211`, the only instance is this one) grows to three read-only columns *Layer / Channels / Used by*, `setIsPersistent(false)`, rebuilt from `snapshot()` on `projectLayersChanged`. `KnobGuiLayers::addNewUserEntry` (`Gui/KnobGuiTable.cpp:647-690`) routes to `Project::addLayer`; `editUserEntry` (`:692-744`) is removed (no edit in M38); the remove button routes to `Project::removeLayer` and shows the refusal dialog with the referencing nodes; a "Remove unused" button is added. `NewLayerDialog` (`Gui/NewLayerDialog.cpp:245-318`) is reused as is, with its sanitizer switched from `makeNameScriptFriendlyWithDots` (`Engine/AppManager.cpp:3772-3808`, which silently rewrites and allows dots) to `LayerRegistry::validate` with the error shown verbatim instead of silently changing what the user typed. `KnobGuiChoice::onItemNewSelected` (`:312-340`) calls `Project::addLayer` and, on success, `setValueFromID`.

**Viewer**: present-only (§3). Its combo is rebuilt from the existing `availableComponentsChanged` path; no registry signal needed.

## 7. Deep

Out of scope for M38 (M60), with one requirement the registry satisfies now: **channel grouping into layers uses the same registry and the same rule as flat images.** 38.1 ships `LayerRegistry::groupChannelNames(const std::vector<std::string>& flat, std::vector<ImageLayerDesc>* layers)` — split at the last `.`; a bare `R/G/B/A/I/Y` → Color; bare `Z` → `depth`; `ZBack` → `depth`-adjacent deep-only channel left to M60; unknown bare names → single-channel layers named after the channel — matching ReadOIIO's classification (`ReadOIIO.cpp:1138-1172`) so a `diffuse.R` in a deep EXR lands on the same `diffuse` registry entry as in a flat one. M60 calls it from `DeepRead` and registers the result through `Project::addLayer(…, eOriginFile)` on the main thread (`registerProducedLayers` must therefore not be gated on `outputKind == eDataKindImage`).

## 8. Risks

- **Auto-registration at load marks the project modified** when a Read's file grew a layer since the save (the union at `ProjectPrivate.cpp:236` happens under `isLoadingProject`, and autosave sees a changed registry). Accepted; it is the truthful state. Test pins that a load with unchanged files makes *no* registry change.
- **ID collisions between files** (`diffuse` RGB vs `diffuse` RGBA): handled by union (§1.3); the >4 case is the residual and is logged, not hidden.
- **Registry bloat**: file-origin entries accumulate; "Remove unused" is the mitigation, the `origin` field is what makes it safe to offer.
- **`_userComponents` removal**: no shipped PyPlug is affected (grep above); third-party PyPlugs calling `addUserLayer` break — clean-break policy applies and the exporter change makes new exports self-contained.
- **OFX plugins caching plane lists**: ReadOIIO caches its own file's layer menu (`ReadOIIO.cpp:1248-1260`) — unaffected. Nothing in openfx-misc/openfx-io reads `kNatronOfxExtraCreatedPlanes`; `kFnOfxImageEffectPropComponentsPresent` is computed per query. A third-party plugin that copies `ComponentsPresent` at `createInstance` would already be stale today on any reconnect.
- **Used-by completeness**: if a reference type is missed (a future knob, a PyPlug alias), removal succeeds and leaves a dangling ID; the failure mode is the §1.2 one (row visible, warned, resolves to nothing), never a crash or a wrong render. `Node::getReferencedLayerIDs()` is the single virtual every new layer knob must feed.
- **Render-thread reads during load**: the viewer can render while the registry is being restored; `snapshot()` makes that safe by construction, and the restore happens before any node exists anyway.

## 9. Open questions — answered 2026-09-19

All three answered as recommended: removal refused while referenced plus "Remove unused"; `depth [Z]` pre-registered; Python surface on `App`.

Original questions for the record:

1. **Removal policy** — refuse-while-referenced with "Remove unused" (recommended), or Nuke's never-remove? Recommendation stands: same safety, plus an exit from bloat.
2. **Pre-register `depth [Z]`?** Recommended yes: ReadOIIO already names lone `Z` that way (`ReadOIIO.cpp:326`), Nuke users expect `depth.Z`, and a ZDefocus-style node wants a target before any Read exists.
3. **Python surface** — `app.addProjectLayer/getProjectLayers/removeProjectLayer` on `App` (recommended: consistent with `getViewNames`, no new wrapper class) versus introducing `app.getProject()` returning a `Project` wrapper just for layers. If a `Project` wrapper is wanted for other reasons later, these become forwarding methods.

## Phase 38.1: Project-level layer registry

- [ ] M38.P1.T1 — Add `LayerRegistry` with validation, reserved aliases, union rule and channel grouping
  - files: `Engine/LayerRegistry.h`, `Engine/LayerRegistry.cpp`, `Engine/CMakeLists.txt`, `Tests/LayerRegistry_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: value class per §1.1 (built-ins seeded in the `Project.cpp:1096-1101` order, `depth [Z]` as `eOriginUser`), `add/remove/find/contains/snapshot`, `validate(desc, fromFile, error)` with `kLayerMaxChannels = 4`, `reservedAlias`, union-on-file-conflict returning an `eAddResult {added, unchanged, grown, refused}`, `groupChannelNames` mirroring `ReadOIIO.cpp:1138-1172`. No Qt signal here (pure data + `QMutex` + `shared_ptr` snapshot).
  - verify: `ctest -R LayerRegistry`: EXPECT_* on built-in order, `rgba`→Color alias, `none/all/Backward` refused, dotted ID refused unless `fromFile`, 5 channels refused, duplicate identical → unchanged, file union RGB+RGBA → grown, user conflict → refused, remove built-in refused, `groupChannelNames({"R","G","B","A","Z","diffuse.R","diffuse.G"})` → Color, depth, diffuse; snapshot taken before an `add` is unchanged after it.
  - size: M

- [ ] M38.P1.T2 — Make `Project` own and serialize the registry; emit `projectLayersChanged`
  - files: `Engine/ProjectPrivate.h`, `Engine/Project.h`, `Engine/Project.cpp`, `Engine/ProjectSerialization.h`, `Tests/ProjectSerialization_Test.cpp`
  - approach: `ProjectPrivate::layers` replaces `defaultLayersList` as the source of truth (the knob stays for T6, marked non-persistent); `Project::addLayer/removeLayer/getLayerRegistry/getLayerUsers`; delete `getProjectDefaultLayers/addProjectDefaultLayer/getProjectDefaultLayerNames` (`Project.cpp:1478-1580`) and the `defaultLayersList` branch of `knobChanged` (`:1776-1781`); `ProjectSerialization::_layers` (`Layers`, non-built-ins only, with `Origin`), version 6→7, restored in `ProjectPrivate::restoreFromSerialization` after formats (`ProjectPrivate.cpp:141`) and before knobs; signal `projectLayersChanged()` (`Project.h:392` pattern), suppressed while `isLoadingProject` and emitted once after `ProjectPrivate.cpp:236`; on change call `refreshChannelSelectors()` on all nodes.
  - verify: extend `ProjectSerialization_Test`: add `diffuse` + `specular`, save, reset, load → registry has both in order, built-ins not written to the XML (grep the saved file), `projectLayersChanged` emitted exactly once on load (QSignalSpy). Independent of T3: uses only `Project` API.
  - size: M

- [ ] M38.P1.T3 — Route availability and OFX publication through the registry; delete per-node user components
  - files: `Engine/EffectInstance.cpp`, `Engine/EffectInstance.h`, `Engine/Node.cpp`, `Engine/Node.h`, `Engine/NodePrivate.h`
  - approach: `getAvailableLayers` (`EffectInstance.cpp:4496-4517`) merges one registry snapshot for `inputNb == -1` only; new `getPresentLayers` (produced ∪ pass-through, `:4466-4493, 4511`); `getUserLayers` (`:182-196`) = registry snapshot; `getComponentsNeededDefault` (`:4288-4297`) uses the registry; delete `Node::addUserComponents/getUserCreatedComponents` (`Node.cpp:7714-7763`), `createdComponents` + mutex (`NodePrivate.h:455-456`), `Node.h:1329-1331`, `Node.cpp:1481-1484`; the choice-knob "New" path temporarily calls `Project::addLayer` from `KnobGuiChoice` (wired in T6).
  - verify: existing `ctest` (WriteAllLayers, TypedPassthrough, DataKind) green; new case in `Tests/WriteAllLayers_Test.cpp`: after loading `flat-three-layers.exr` through ReadOIIO, `getPresentLayers(-1)` on the Read = {Color, diffuse, specular} and `getAvailableLayers(-1)` ⊇ registry built-ins; a Blur downstream reports present = the same three. Builds without `NodeSerialization` changes (T4 does those).
  - size: M

- [ ] M38.P1.T4 — Drop `UserComponents` from node serialization and the PyPlug exporter
  - files: `Engine/NodeSerialization.h`, `Engine/NodeSerialization.cpp`, `Engine/NodeGroup.cpp`
  - approach: remove `_userComponents` (`NodeSerialization.h:236-238, 267, 321, 430`, `.cpp:202`), bump `NODE_SERIALIZATION_CURRENT_VERSION` (`:73`); remove the `addUserLayer` emission loop (`NodeGroup.cpp:2733-2745`); no migration (clean break, §1.2).
  - verify: `ctest -R ProjectSerialization` and `DataKindProjectLoad` green; a saved `.ntp` contains no `UserComponents` element (test greps the file); `Tests/fixtures/*.ntp` still load.
  - size: S

- [ ] M38.P1.T5 — Auto-register produced layers on the main-thread refresh
  - files: `Engine/Node.cpp`, `Engine/Node.h`, `Engine/Project.cpp`, `Tests/LayerRegistry_Test.cpp`
  - approach: `Node::registerProducedLayers()` called at the end of `refreshAllInputRelatedData` (`Node.cpp:6614` region): produced non-Color planes from `getComponentsNeededAndProduced_public(comps[-1])` → `Project::addLayer(desc, isReader ? eOriginFile : eOriginPlugin)`; union results call `incrementKnobsAge()` on `getLayerUsers(id)`; batched under `isLoadingProject`. `Node::getReferencedLayerIDs()` virtual, implemented over `_imp->channelsSelectors`/`maskSelectors` choice values so `getLayerUsers` is truthful in 38.1.
  - verify: test: create ReadOIIO on `flat-three-layers.exr` → registry gains `diffuse`, `specular` with `eOriginFile`; change the file knob to a plain RGBA PNG fixture → both stay registered; save/reset/load with the file unchanged → registry identical and `projectLayersChanged` count unchanged after load; select `diffuse` on a Blur's Output Layer choice → `removeLayer("diffuse")` returns false and `getLayerUsers` names the Blur.
  - size: M

- [ ] M38.P1.T6 — Project Layers page as a registry view; dialog validation; "New…" writes to the registry
  - files: `Engine/KnobTypes.h`, `Gui/KnobGuiTable.cpp`, `Gui/KnobGuiTable.h`, `Gui/NewLayerDialog.cpp`, `Gui/KnobGuiChoice.cpp`
  - approach: `KnobLayers` → three read-only columns Layer/Channels/Used by, `setIsPersistent(false)`, rebuilt from `snapshot()` on `projectLayersChanged`; `KnobGuiLayers::addNewUserEntry` → `Project::addLayer`, remove → `Project::removeLayer` with refusal dialog listing users, "Remove unused" button; delete `editUserEntry`/`tableChanged` rewriting; `NewLayerDialog::getComponents` validates via `LayerRegistry::validate` and surfaces the message; `KnobGuiChoice::onItemNewSelected` (`:312-340`) → `Project::addLayer` + `setValueFromID`.
  - verify: Xvfb run via `build/m38scout/` (recipe in `build/deeprepro/run-gui.sh`): script opens Project Settings → Layers, screenshots the page with Color…Forward, depth, and the two file layers of a loaded `flat-three-layers.exr` (Used by = 0/1); "New…" on a Blur's Output Layer creates `spec2` and the Layers page shows it; removing `diffuse` while the Blur selects it shows the refusal dialog (screenshot). Viewer combo (`ViewerTabPrivate.cpp:385` switched to `getPresentLayers`) shows only Color/diffuse/specular on the Read.
  - size: L

- [ ] M38.P1.T7 — Python: project layer API on `App`; delete `Effect.addUserLayer`
  - files: `Engine/PyAppInstance.h`, `Engine/PyAppInstance.cpp`, `Engine/PyNode.h`, `Engine/PyNode.cpp`, `Engine/typesystem_engine.xml`
  - approach: `getProjectLayers/getProjectLayer/addProjectLayer(name, channels)/removeProjectLayer` per §6 raising `ValueError` with the registry's message; remove `Effect::addUserLayer` (`PyNode.cpp:991-1009`, `PyNode.h:376`); `ImageLayer` unchanged.
  - verify: a Python script run through the built binary in background mode (`-b` with a `.py`, as `build/m38scout/gui.py` does) asserts: built-ins first, `addProjectLayer("diffuse", ["R","G","B"])` returns an `ImageLayer`, re-adding is idempotent, `addProjectLayer("rgba", …)` returns Color, `addProjectLayer("bad name", …)` raises, `removeProjectLayer("Color")` is False, `hasattr(node, "addUserLayer")` is False.
  - size: S

## Decisions

- 2026-09-19 — **The registry is a `Project`-owned value class, not the `defaultLayers` knob**: the knob re-parses a `tr()`-tagged string on render-adjacent paths (`Project.cpp:1483`, `KnobTypes.cpp:2478-2494`), conflates ID and label (`:1494-1507`), has no uniqueness check and its Python path never refreshes menus (`:1776-1781` vs `KnobImpl.h:1804`); the knob survives only as a non-persistent view.
- 2026-09-19 — **Unique by case-sensitive ID; label == ID for every non-built-in layer; no rename in M38**: EXR names are case-sensitive; one identity keeps regex-on-labels and the dropdown honest; rename is remove + add.
- 2026-09-19 — **Cap at 4 channels, stored as a vector behind one constant**: `bitset<4>` end to end, the OFX decoder (`ImageLayerDesc.cpp:382`) and ReadOIIO (`:364`) all refuse more; lifting it is an image-storage milestone, not a registry one.
- 2026-09-19 — **Serialize the registry in `ProjectSerialization`, restored before nodes; built-ins never written; file-discovered layers written**: same ordering guarantee formats rely on (`ProjectPrivate.cpp:141, 205`); the script remembers its channels like a `.nk` does.
- 2026-09-19 — **Per-node `UserComponents` dropped without migration**: clean-break decision of 2026-09-18; the data is trivially recreated; keeping it would keep per-node layer ownership alive.
- 2026-09-19 — **A node that produces a layer registers it, on the main-thread refresh, and the layer stays registered when the file stops carrying it**: one generic rule covers Read, plugins and future native readers; Nuke behaviour on file change; render threads never mutate.
- 2026-09-19 — **File-origin conflicts union (≤4), user conflicts refuse**: never silently drop a file's channel; never silently rewrite what a user typed.
- 2026-09-19 — **Removal refused while referenced; "Remove unused" for bloat; built-ins immutable**: Nuke's safety without Nuke's accretion.
- 2026-09-19 — **Input-bound knobs and the viewer list present layers only; target knobs (Roto output, generators, Shuffle output) list the registry and carry "New layer…"** (user decision, replacing the greyed-entries proposal): greyed registry entries in every input list are visual clutter; a layer only needs to be targetable where a node can write into it.
- 2026-09-19 — **No hash or cache-version change for add/remove; union bumps referencing nodes' knobs age**: `ImageKey` has no plane (`ImageKey.cpp:68-79`), `ImageParams::_components` disambiguates; only a channel-count change can alter pixels.
- 2026-09-19 — **Python surface lives on `App`** (`addProjectLayer/getProjectLayers/removeProjectLayer`): no `Project` wrapper exists (`PyAppInstance.h:306`); `Effect.addUserLayer` deleted.
- 2026-09-19 — **Deep shares the registry through `groupChannelNames`**: M60 wires DeepRead; the grouping rule is written once, mirroring ReadOIIO (`:1138-1172`).
