# Design: the layer/channel selection widget (M38)

2026-09-19. Draft for approval. Supersedes the v1 consultant report (appended below as an annex) §A.1–A.5 where they differ; v1's engine and render citations are carried over unchanged unless restated here. Governing user direction: a Nuke-style widget, more powerful (regex, add/remove rows); nodes process exactly the selected channels of the selected layers and no others; non-Shuffle nodes never shuffle; one consistent channel-selection UI with small, named variations; Merge on a single-row/no-regex variant. Decided elsewhere: clean break for old projects (`docs/decisions/2026-09-18-clean-break-no-project-compat.md`); regex on layer *labels*, anchored, case-sensitive, `QRegularExpression`; ZRemap/ZMask dropped until M34; one undo step per user action; Read's "Output Components" → "Output Channels" via the openfx-io fork.

## Goals

1. **One component.** A single knob type, `KnobLayerChannels` (script name `channels`, label "Channels"), and a single GUI, `KnobGuiLayerChannels`, with named presets expressed as feature flags. No second widget type for Merge, Premult, or PyPlugs.
2. **One meaning.** A row says "process these channels of this layer". Unselected channels are copied through from the preferred input's *same* layer; unselected layers pass through untouched. No node other than Shuffle moves data between layers or channels.
3. **Plugin adoption without plugin changes.** The openfx-misc plugins that declare their own R/G/B/A booleans (28 with the standard `NatronOfxParamProcess*` names; Merge with `AChannels*`/`BChannels*`/`OutputChannels*`) are folded into the widget by the host, by name. The pinned openfx-misc ref in `tools/ci/local/fetch-assets.sh:235` is untouched.

Non-goals: the native Shuffle (M34); the premult warning's future (M43 deletes it); any project migration.

## 1. The widget family

### 1.1 Schema (identical across variants)

`KnobLayerChannels` derives from `KnobTable` (`Engine/KnobTypes.h:1079-1140`; `KnobLayers` at `:1142-1211` is the model). Three columns, one row per widget instance:

| column | content |
|---|---|
| `mode` | `none` \| `all` \| `layer` \| `regex` |
| `layer` | layer ID (`ImageLayerDesc::getLayerID()`, `Engine/ImageLayerDesc.h:110`; Color = `kNatronColorLayerID` `:55`) for `layer` rows; the pattern verbatim for `regex` rows; empty otherwise |
| `channels` | comma-joined enabled channel *names* (`layer` rows only), always a subset of that layer's `getChannels()` (`:121`) |

Serialised as one table string via the existing KnobTable path (`Engine/KnobSerialization.cpp:158-165`, factory `Engine/KnobFactory.cpp:92-95`). **Variant flags are not serialised**: they are a property of the node's plugin, re-derived at every node creation, so a `.ntp` never encodes them and cannot go stale. Type mismatch with an old `channels` Choice knob is already skipped by the guard at `Engine/Node.cpp:1625-1628` — the clean-break decision means nothing more is done.

### 1.2 Flags and presets

```cpp
struct LayerChannelsVariant {
    int  maxRows;          // 1 or kUnbounded
    bool allowRegex;       // Regex… entry in the dropdown
    bool showLayerDropdown;// false => mode is always `layer`, layer set by the host
    bool showChannelButtons;
};
```

Four named presets; nothing else is constructible from the host rule (Python can read `getVariant()` but not set it):

| preset | maxRows | regex | dropdown | buttons | who it is for |
|---|---|---|---|---|---|
| `full` | ∞ | yes | yes | yes | ordinary filters: Blur, Grade, Invert, ColorCorrect, … (default) |
| `layers` | ∞ | yes | yes | no | plugins that declared `ChannelSelector = None` and have no channel quad: Transform, Switch, TimeOffset, Constant(solid), … |
| `single` | 1 | no | yes | yes | nodes whose channel semantics live in a plugin-owned quad that cannot hold per-layer sets: Merge |
| `channels` | 1 | no | no | yes | the layer is decided elsewhere: multiplanar plugins with a standard quad (Premult, Unpremult, STMap, IDistort), RotoPaint, and every adopted *input* quad row (Merge "A"/"B") |

Invariants the knob enforces (and the Python API surfaces as exceptions): `addRow` beyond `maxRows` rejected; `regex` mode rejected when `!allowRegex`; `none`/`all` and layer changes rejected when `!showLayerDropdown`; channel edits rejected when `!showChannelButtons`. A `single` is literally a `full` capped at one row with regex disallowed — same code, same serialisation.

### 1.3 How a node gets its variant

Precedence, first match wins:

1. **OFX descriptor property** `NatronOfxImageEffectPropLayerChannelsVariant` (string: `"none" | "full" | "layers" | "single" | "channels"`, default `""`). Registered next to `kNatronOfxImageEffectPropChannelSelector` in the OpenFX fork's descriptor table (`libs/OpenFX/HostSupport/src/ofxhImageEffect.cpp:139` and `:4755`) and in `libs/OpenFX/include/ofxNatron.h:256`; advertised by the host at `Engine/OfxHost.cpp:328`. This is the opt-in for plugin authors; no openfx-misc plugin needs it.
2. **Native nodes**: new field `NativePluginDescription::layerChannels` (`Engine/Nodes/NativeEffectBase.h:68-91`), enum `{eNone, eFull, eLayers, eSingle, eChannels}`, default `eFull` for `outputKind == eDataKindImage`, forced `eNone` otherwise (this also removes DeepMerge's stray host rows, `build/m38scout/dump.txt:368-375`).
3. **Not eligible → `none`**: reader, writer, tracker, furnace, non-image output kind (v1's `getCreateLayerChannelsKnob`, replacing `EffectInstance.cpp:4522-4526`), and multiplanar plugins *without* a standard quad (Shuffle, RotoMerge-less paths).
4. **Multiplanar with a standard output quad** → `channels` (Premult/Unpremult `Premult.cpp:976-979`; STMap/IDistort `Distortion.cpp:3481`). The plugin picks the layer (`inputPlane`); the host provides only the buttons.
5. **Has an input quad** (a quad whose prefix is not `NatronOfxParamProcess`/`OutputChannels`) → `single`. Today that is exactly Merge (`Merge.cpp:181-227`; grep of openfx-misc finds no other `<X>ChannelsR` family).
6. **Has an output quad, or `ChannelSelector != None`** → `full`.
7. **Otherwise** (`ChannelSelector == None`, no quad) → `layers`.

### 1.4 Quad recognition and adoption (the generic rule)

A **quad** is four `KnobBool`s whose original names are `<P>R`, `<P>G`, `<P>B`, `<P>A`, where `<P>` is `NatronOfxParamProcess` or any identifier ending in `Channels`. Recognition happens once in a new `Node::adoptChannelQuads(mainPage)` replacing `findOrCreateChannelEnabled` (`Engine/Node.cpp:2632-2707`).

- **Output quad** (`<P>` ∈ {`NatronOfxParamProcess`, `OutputChannels`}): its four defaults seed the Color row's channel set (Grade's `A` off, `Grade/Grade.cpp:1347`, survives). The bools become secret and non-persistent. Ownership of masking follows §4.
- **Input quad** (any other `<P>`, e.g. `AChannels`): the host creates a second `KnobLayerChannels` in the `channels` preset, script name `channels_<P minus "Channels">` (→ `channels_A`, `channels_B`), label `<P minus "Channels">` (→ "A", "B"), inserted directly under the Channels row. The bools are secret, non-persistent, and **mirrored** from the row (§1.5). The plugin keeps ownership: "zero this input's channel before merging" is not something the host can emulate after the fact.
- **Labels and dividers**: a `String` param named exactly `<P>` with `eStringTypeLabel` (Merge's `AChannels`/`BChannels`/`OutputChannels`, `Merge.cpp:1765-1772`) is set secret; the host-synthesised divider after the last bool (`OutputChannelsA_separator`, `dump.txt:67`) is set secret.
- **No quad, `ChannelSelector != None`**: the host no longer creates `NatronOfxParamProcess*` bools; the property's value seeds the Color row's default set (`Engine/OfxEffectInstance.cpp:3068-3096`).

### 1.5 The mirror rule (widget → positional bools)

For every adopted quad the host holds, per render-relevant change, writes the four bools from the driving row. Let `L` be the driving layer with channels `c₀…c_{N-1}` and `S` the row's enabled set:

- `N ≥ 2`: `bool[i] = (cᵢ ∈ S)` for `i < N`; positions `≥ N` are written **true**. For an RGB or XY layer this leaves `bool[3] = true`, which is what Merge needs to treat a no-alpha input as opaque (`Merge.cpp:481, 507`).
- `N == 1`: the single channel's state is written to **both** position 0 and position 3; positions 1, 2 are true. Merge's A/B quads read position 0 for one-channel images (`Merge.cpp:464-465`) while its Output quad reads position 3 (`:435, :604`) and the host's own bitset convention is 3 (`Engine/ImageCopyChannels.cpp:567-585`); writing both makes every convention read the same value.
- `all` or `regex` mode on the driving row: all four true.
- `none`: all four true (the node is identity anyway, §4).

The driving layer of a `channels`-preset knob is: the node's Channels row (row 0) if the node has one; otherwise Color with fixed labels R G B A (multiplanar plugins, RotoPaint). When the driving layer changes, the `channels` row **resets to all channels of the new layer** — the same rule the dropdown applies to its own row (§3.4) — so a stored set is always a subset of its current layer.

## 2. Merge

### 2.1 What the plugin actually declares

`Merge.cpp:1760-1916`: `setChannelSelector(ePixelComponentNone)`, `setIsMultiPlanar(plugin == eMergePluginRoto)` (so the user-facing Merge is *not* multiplanar), three label strings (`AChannels`, `BChannels`, `OutputChannels`), three bool quads, a divider hint on `OutputChannelsA`, two vestigial secret bools `aChannelsChanged`/`bChannelsChanged` (declared `:1922-1940`, never read). The host today adds `B_channels`/`A…_channels` layer choices, `channels` "Output Layer" and `processAllLayers` (`dump.txt:70-136`) and no RGBA rows.

Render semantics: A/B bools zero the corresponding input channel before `mergePixel` (`:464-465, :538`); for RGB/XY images bool[3] decides whether that input's alpha is 1 or 0 (`:481, :507, :553`); Output bools copy B's value into unchecked output channels after mask/mix (`:602-607`) and drive `isIdentity` (`:1313-1333`) and the Roto variant's `getClipComponents` (`:840-847`).

### 2.2 Decision

- Merge's main knob `channels` is the **`single`** preset (rule 5 above). Its channel buttons are the **Output quad**: the buttons write `OutputChannels{R,G,B,A}` via the mirror rule; the quad is secret and non-persistent; the row is the persisted truth.
- `AChannels*` and `BChannels*` become two **`channels`**-preset knobs `channels_A` / `channels_B`, labelled "A" and "B", placed at rows 1 and 2 directly under the Channels row (not where the plugin put them, which was after "Alpha masking"). Their buttons are labelled with the selected layer's channel names, mapped by position.
- **Masking owner: the plugin.** The host performs no `copyUnProcessedChannels` on Merge (`hostChannelSelectorEnabled` stays false; `Node::getProcessChannel` keeps returning true). Reason: a `single` node has exactly one channel set, which the plugin's quad can hold exactly, and the plugin's `isIdentity` and "copy from B, or zero if B is disconnected" (`:605`) are better than the host's "copy from the preferred input", which would silently switch to A when B is disconnected (`Engine/NodeInputs.cpp:1404-1455`).
- **No openfx-misc change.** Everything above is host adoption by name. The two vestigial `*ChannelsChanged` bools stay secret and are ignored.
- **Layer selection and the no-shuffle invariant** hold as for every node: Merge on layer `diffuse` reads `diffuse` from B and every A, writes `diffuse`; other layers pass through from B (`ePassThroughPassThroughNonRenderedLayers` behaviour is unchanged).

### 2.3 Multi-channel-count layers

| Channels row | A/B rows show | Output mirror | A/B mirror |
|---|---|---|---|
| Color (RGBA) | [R][G][B][A] | positions 0-3 | positions 0-3 |
| diffuse (RGB) | [R][G][B] | 0-2, bool[3]=true | 0-2, bool[3]=true → A's alpha treated as 1 |
| motion (XY) | [X][Y] | 0-1, 2-3 true | 0-1, 2-3 true |
| depth (Z) | [Z] | positions 0 and 3 | positions 0 and 3 |
| All | [R][G][B][A] fixed | all true | positional, applies to every layer alike |
| None | rows disabled (greyed) | all true | all true; node is identity |

### 2.4 Merge panel mockup

```
Channels   [ Color    ▾ ] [R] [G] [B] [A]  ⚠            ← single: no [+], no Regex…
A          [R] [G] [B] [A]                              ← channels_A (AChannels*)
B          [R] [G] [B] [A]                              ← channels_B (BChannels*)
Operation  [ over ▾ ]        Bounding Box [ union ▾ ]
Alpha masking  □
──────────────────────────────────────────  ← advancedSep
Mask       ☑ [ Mask.A ▾ ]   □ Invert Mask
Mix        [ 1.00 ]
```
With `diffuse` selected the three rows read `[R] [G] [B]`; with `motion` they read `[X] [Y]`. The ⚠ is the existing premult warning, now inline after the Channels row (§3.2).

## 3. Consistency rules

### 3.1 One label, one row shape, one palette
- Label "Channels" for row 0 on every node that has it; input rows use the input's short name ("A", "B").
- A row is: `[dropdown] [buttons…] [−]` (or `[buttons…]` for `channels`; `[dropdown]` only for `layers`). Buttons are checkable `Button`s with the colours from `Gui/KnobGuiBool.cpp:280-298` for R/G/B/A by *name* (so `diffuse.R` is red, `motion.X` and `depth.Z` neutral). The button text is the channel's short name; the tooltip is `layer.channel`.
- Dropdown contents, in order: `None`, `All`, `Regex…` (if allowed), separator, `Color`, then the other available layers sorted by label. "New…" is not in this widget (M36).
- Row 0 sits at main-page index 0 (`mainPage->insertKnob(0, …)` in `initializeDefaultKnobs`, `Engine/Node.cpp:2785-2795`); adopted input rows follow immediately; the plugin's own params start after them. `advancedSep` logic (`:2842-2867`) is untouched.

### 3.2 Node-level behaviours
- **Cannot animate, cannot take an expression.** `canAnimate()` is false, no keyframe affordances, `setExpression` refused (the knob is a table string; a per-frame channel set has no render meaning we want to support). Linking/aliasing is allowed (§3.3).
- **Disabled / read-only**: the whole widget greys out; the summary label (below) still updates.
- **Premult warning** (`Node.cpp:2691-2706`, `:7545-7600`): inserted at index 1 with row 0's `setAddNewLine(false)`, re-based on row 0's Color set (alpha on, some of RGB off). M43 deletes it.
- **NodeGui summary** (`Gui/NodeGui.cpp:3190-3230`): row 0 rendered as `Color.rgb`, `diffuse`, `All`, `/spec.*/`, `None`; multiple rows joined with `, `; multiplanar branch kept (`Node.cpp:7526`).
- **Viewer**: unaffected — it only consumes `getAvailableLayers` (`Gui/ViewerTab40.cpp:857-995`).

### 3.3 Aliased params in a PyPlug group
`Effect.createLayerChannelsParam(name, label)` on the group + `setAsAlias(inner.getParam("channels"))` (generic `setKnobAsAliasOfThis`, `Engine/Knob.h:1250`). The alias inherits the master's variant flags and delegates `getAvailableLayers()` to the master's node (the group node has no layer list of its own). The alias widget is pixel-identical. The seven shipped PyPlugs that alias `NatronOfxParamProcess*` bools (`EdgeBlur.py:482-483` and friends) are rewritten to alias `channels`, since the bools are now secret and, on `full` nodes, forced true.

### 3.4 Keyboard and undo
- Tab order per row: dropdown → buttons left-to-right → `[−]`; then next row; `[+ Add layer]` last. Space toggles a focused button. In the regex editor Enter commits, Esc reverts to the last committed pattern.
- **One undo step per user action**: button toggle, dropdown change, regex commit (`editingFinished`), add row, remove row. Implemented as `KnobUndoCommand<std::string>` (`Gui/KnobGui.h:136`) with a new `setMergeable(false)` (default `_merge(true)`, `Gui/KnobUndoCommand.h:85`, `mergeWith` `:289-306`). A dropdown change that resets dependent rows (its own buttons, and any adopted input rows) pushes one macro command carrying all affected knobs' old/new values, so Ctrl+Z restores the panel as it was in a single step.
- Choosing a layer in the dropdown resets that row's buttons to all-on (Nuke behaviour; the plugin's default set applies only to the initial Color row).

### 3.5 Where the widget is absent, and what is shown instead
| node | shows | why |
|---|---|---|
| Read | its own "Output Channels" choice (label change via the openfx-io fork) | a reader chooses what to decode, not what to process |
| Write | its own layer choice + "All Layers" (M57/M58) | a writer chooses what to encode |
| Shuffle | `outputLayer`, `outputR/G/B/A` | it *is* the shuffle; replaced natively in M34 |
| Tracker | nothing | analysis node, no image processing |
| Viewer | nothing (its layer selectors are in the viewer toolbar) | display, not processing |
| Deep nodes | nothing | `outputKind != image` |
| RotoPaint / Roto | `channels` preset, four buttons R G B A | "which channels to paint into"; layer is always Color |

## 4. Engine and render model (delta over v1)

Carried over verbatim from v1 §A.1–A.2: resolution order and de-duplication; Color moved to front for premult (`Engine/EffectInstanceRenderRoI.cpp:662, 1283`); bit mapping `c→bit c`, `N==1→bit 3`; `getComponentsNeededDefault` (`Engine/EffectInstance.cpp:4219-4372`) fed from `node->resolveLayerSelection`; deletion of `processAllRequested`/`processChannels` out-params (`:4375-4450`, `EffectInstance.h:1983-2004`, `EffectInstancePrivate.h:70,130-133`, dead block `EffectInstanceRenderRoI.cpp:457-476`); per-plane input fetch via `findEquivalentLayer` (`ImageLayerDesc.h:201-226`); identity via `hasAtLeastOneChannelToProcess(time, view)` and deletion of "choice B" (`EffectInstanceRenderRoI.cpp:630-680`); masks untouched.

**Masking owner — the one new rule.** Host masking (`copyUnProcessedChannels`, `EffectInstance.cpp:2795, 2872`, per-plane `originalInputImage`/bitset chosen in `renderHandler`'s loop `:2250-2258, :2618`) runs **iff** the node's variant is `full` or `layers`, or the node has no adopted output quad. Otherwise (`single`, `channels`) the plugin masks from its mirrored quad and the host bitset is all-true. Rationale: a multi-row widget holds per-layer sets that a single positional quad cannot express, so the host must own masking there and the quad is forced all-true; a one-row widget's set fits the quad exactly, and letting the plugin mask keeps its own identity/disconnected-input semantics intact. Concretely for the 28 standard plugins (all `full`): bools forced true, host masks — this moves masking from plugin to host for those nodes, which is a behavioural change verified by 38.2.T4. For Premult/Unpremult/STMap/IDistort (`channels`): plugin masks, unchanged from today.

`resolve()` cost: called inside `getComponentsNeededDefault`, whose result is already cached per hash by ActionsCache; the compiled `QRegularExpression` is cached on the knob and swapped under a mutex on value change; available-layer lists are tens of entries. It is not on the per-tile path.

`NATRON_CACHE_VERSION` 5→6 (`Global/GlobalDefines.h:97`).

## 5. Python API

One class, `LayerChannelsParam` (`Engine/PyParameter.h`, factory `Engine/PyNode.cpp:398-456`), for every variant:

```python
p = node.getParam("channels")
p.getVariant()            # "full" | "layers" | "single" | "channels"
p.getRowCount()
p.getRow(i)               # ("layer", "diffuse", ["R","G"]) / ("regex", "spec.*", []) / ("all", "", []) / ("none", "", [])
p.setNone(row=0); p.setAll(row=0)
p.setLayer("diffuse", channels=None, row=0)   # channels=None => all channels of that layer
p.setChannels(["R","G"], row=0)
p.setRegex("spec.*", row=0)
p.addLayer("specular", channels=None)          # appends; returns new row index
p.addRegex("spec.*")
p.removeRow(i)                                 # i >= 1
p.getAvailableLayers()                         # [("Color", ["R","G","B","A"]), ("diffuse", [...]), ...]
p.resolve()                                    # [("Color", ["R","G","B"]), ...] — what the render will process now
```
Violations raise `ValueError` with the variant named: `addLayer`/`addRegex` on `single`/`channels`; `setRegex` when `!allowRegex`; `setLayer`/`setNone`/`setAll` on `channels`; `setChannels` on `layers`; unknown channel name for the row's layer; `removeRow(0)`. `Effect.createLayerChannelsParam(name, label)` exists for PyPlug groups (variant copied from the alias master on `setAsAlias`). Removed: `getParam("channels")` as ChoiceParam, `processAllLayers`, `<Input>_channels`, host-created `NatronOfxParamProcess*`; plugin-declared quads remain reachable as secret `BooleanParam`s but writing them is pointless (the mirror overwrites).

## 6. Risks

- **Render-path regressions**: replacing the single-bitset host masking with per-plane masking and moving masking ownership for the 28 standard plugins touches `renderHandler`. Guarded by 38.2.T4 (pixel tests on `flat-three-layers.exr`) and the release-AppImage checkpoint.
- **Forced-true bools change plugin behaviour beyond masking.** Any of the 28 plugins that reads `processX` to alter its *algorithm* (not just mask) would change output. Audit in 38.2.T4 covers Grade, Invert, ColorCorrect, Blur, Saturation, Multiply; the remainder is spot-checked from the openfx-misc grep list. Setting the bools at creation also fires `paramInstanceChanged` into the plugin — harmless for these plugins, but noted.
- **Adoption by name is a convention, not a contract.** A third-party plugin with a `FooChannelsR` quad that means something else would be adopted as an input row. Mitigation: the descriptor property (§1.3.1) opts out (`"none"`) or pins a preset; the rule is documented in `ofxNatron.h`.
- **Merge's 1-channel quirk** (§1.5) is handled by writing positions 0 and 3; a future openfx-misc bump that changes those indices would need the mirror updated. Test 38.2.T3 pins it.
- **PyPlug breakage**: seven shipped PyPlugs script the old bools; they are rewritten in 38.4. Third-party PyPlugs doing the same will silently lose their aliases — clean-break policy applies.
- **Undo macro across knobs** (dropdown → dependent resets) is the one place a single user action edits several knobs; if the macro is wrong the panel desynchronises on undo. Covered by the Xvfb harness test in 38.3.T1.
- **OpenFX submodule bump** (38.1.T5) is a one-line table entry, but it is a submodule pointer change in a user-owned fork; if it slips, everything else still works because no shipped plugin sets the property.

## 7. Open questions

1. **`layers` preset for `ChannelSelector == None` plugins without a quad (Transform, Switch, …): buttons or no buttons?** Recommendation: no buttons — the plugin explicitly declined a channel selector, and host-masking a Transform per channel is technically possible but semantically odd. Cheap to flip later (one flag).
2. **Reset-on-layer-change for adopted input rows (Merge A/B).** Recommendation: reset to all-on, matching the Channels row and Nuke; the alternative (keep by position) preserves a user's `A: R only` across Color→diffuse but produces surprising states on XY/Z layers.
3. **RotoPaint/Roto in M38 or later?** Recommendation: in M38 as one S task (38.4.T5) — the four bools already exist, the mirror rule makes RotoPaint's `_imp->enabledKnobs` reads (`Engine/RotoPaint.cpp:199-215, 1365`) work with zero engine change, and it closes the last visible inconsistency.

---

---

# Annex: v1 consultant report (engine/render citations referenced above)


## A.1 Engine model
One new knob type `KnobLayerChannels`, script name `channels`, label "Channels", derived from `KnobTable` (`Engine/KnobTypes.h:1079-1140`, like `KnobLayers` `:1142-1211`); columns Mode/Layer/Channels, one row per widget instance. Mode ∈ none|all|layer|regex. Layer column = layer ID (`ImageLayerDesc::getLayerID()`, `Engine/ImageLayerDesc.h:110`; Color = `kNatronColorLayerID` `:56`) or the regex pattern verbatim. Channels column = comma-joined enabled channel names (Layer mode only). Serializes as one string knob value; type registered in `Engine/KnobSerialization.cpp:158-165` and `Engine/KnobFactory.cpp:92-95`.
Typed API: `getRows/setRows/setAvailableLayers/getAvailableLayers/resolve/isPatternValid/getSummaryLabel`, signal `availableLayersChanged()`. Compiled `QRegularExpression` cache swapped under a QMutex on value change.
Resolution: rows in order, de-duplicated by layer ID; none→nothing; all→every available layer, every channel; layer→that ID with bit per enabled channel name the layer actually has (unavailable→nothing); regex→every available layer whose *label* matches, anchored (`QRegularExpression::anchoredPattern`), case-sensitive; invalid→nothing. Same layer in two rows → OR of channel sets. Color moved to front (render path reads `outputComponents.front()` for premult, `Engine/EffectInstanceRenderRoI.cpp:662, 1283`). Bit mapping: N-channel layer channel c→bit c, except N==1→bit 3 (`Engine/ImageCopyChannels.cpp:567-585`).
All/Regex: channel buttons hidden, every channel processed. Empty selection → identity via `hasAtLeastOneChannelToProcess()` (`Engine/EffectInstance.cpp:3758`).
Default: one `layer` row = Color with the plugin's default channels (`kNatronOfxImageEffectPropChannelSelector`, `Engine/OfxEffectInstance.cpp:3068-3096`; or the 28 openfx-misc plugins' own `NatronOfxParamProcess*` defaults, e.g. Grade A off `Grade/Grade.cpp:1347`); native RGBA (`Engine/EffectInstance.h:1860-1871`); `ePassThroughRenderAllRequestedLayers` effects default to one `all` row (`Engine/Node.cpp:2973-2979`).

## A.2 Render semantics
Current: `getComponentsNeededDefault` (`Engine/EffectInstance.cpp:4219-4372`) sets comps[-1]=Output Layer, comps[i]=per-input selector; `renderRoI` (`EffectInstanceRenderRoI.cpp:492-524`) renders planes in comps[-1], else pass-through. `clipGetImage` returns `neededComps[input].front()` regardless of plane (`Engine/OfxClipInstance.cpp:853-874`); host masking `copyUnProcessedChannels` (`EffectInstance.cpp:2795, 2872`) copies from `imgs[preferredInput].front()` (`:2256-2257`) with one bitset for all planes — that IS the shuffle.
New: (1) comps[-1]/comps[i] from `node->resolveLayerSelection` (Color→metadata comps; empty→metadata comps; masks unchanged via `getMaskChannel` `:4338`); (2) delete `processAllRequested`/`processChannels` out-params of `getComponentsNeededAndProduced_public` (`:4375-4450`, `EffectInstance.h:1983-2004`, ActionsCache `EffectInstancePrivate.h:70,130-133`) and dead block `EffectInstanceRenderRoI.cpp:457-476`; callers `EffectInstanceRenderRoI.cpp:450`, `OutputSchedulerThread.cpp:2297`, `RotoSmear.cpp:243`, `EffectInstance.cpp:4476`; (3) per-plane input fetch: `getInputImageInternal` picks the `neededComps[input]` entry equivalent to `outputLayerBeingRendered` (`ImageLayerDesc::findEquivalentLayer` `ImageLayerDesc.h:201-226`), fallback front (masks); analysis fallback `:876-888` uses resolved selection; (4) per-plane host masking: choose `originalInputImage` and bitset per plane in `renderHandler`'s loop (`EffectInstance.cpp:2725`, `:2250-2258`, `actionArgs.processChannels` `:2396`, loop `:2618`); (5) identity: `hasAtLeastOneChannelToProcess(time, view)` (`Node.cpp:5845`); delete "choice B" path `EffectInstanceRenderRoI.cpp:630-680`; (6) regex/all resolved inside `getComponentsNeededDefault`, cached per hash by ActionsCache.
Per-channel enables = host masking for every widget node. 28 openfx-misc plugins with standard `kNatronOfxParamProcessR/G/B/A` (e.g. `Invert/Invert.cpp:57-67`): adopt — harvest defaults into the Color row, setSecret, non-persistent, forced true. Merge's `AChannelsR…/BChannelsR…/OutputChannelsR…` (non-standard names): v1 said leave visible; USER NOW WANTS Merge on a widget variant too.
Exempt nodes keep `findOrCreateChannelEnabled` (`Node.cpp:2632-2707`) + `getProcessChannel(i)` (`:5772-5785`); RotoPaint reads `getProcessChannel(3)` (`Engine/RotoPaint.cpp:199-215, 1365`).
No-shuffle invariant: node renders plane L reading plane L from every non-mask input, writes L; unselected channels copied from preferred input's plane L; planes ∉ S pass through. `<Input>_channels` and `channels`(Output Layer) deleted. Masks (`createMaskSelectors` `Node.cpp:2493-2561`, `getMaskChannel` `:7610-7637`, mask loop `:7683-7706`, maskInvert, mix, hostMix) unchanged.
Eligibility: `getCreateChannelSelectorKnob` → `getCreateLayerChannelsKnob`, default `!multiplanar && !reader && !writer && !tracker && !furnace && getOutputDataKind()==eDataKindImage` (`EffectInstance.cpp:4522-4526`, `EffectInstance.h:504-507`); existing false overrides stay; `NativeEffectBase::isHostChannelSelectorSupported` → false (DeepMerge currently shows stray host rows, `build/m38scout/dump.txt:363-376`).

## A.3 Deletions
`Engine/Node.cpp`: `createChannelSelectors` `:2709-2723`, `createChannelSelector` `:2937-2991`, host RGBA creation `:2673-2690`, `getSelectedLayerChoiceRaw` `:5586-5599`, `getSelectedLayerInternal` `:5601-5632`, `onLayerChanged` `:5634-5678`, `refreshEnabledKnobsLabel` `:5680-5740`, `getSelectedLayer` `:5787-5843`, `refreshLayersChoiceSecretness` `:6493-6522`, `getChannelSelectorKnob` `:7519-7537`, `getProcessAllLayersKnob` `:7539-7543`, choice loop in `refreshChannelSelectors` `:7649-7679` (→ `setAvailableLayers`; keep function + `onChannelsSelectorRefreshed` `:7708` for viewer `availableComponentsChanged` `Engine/ViewerInstance.cpp:3359`), knob dispatch `:5507-5512, 5528-5550`, `setProcessChannelsValues` `:569-592` (re-impl on Color row; caller `:6959`), `addUserComponents` `:7714-7755` appends a Layer row. `NodePrivate.h` `ChannelSelector` `:48-55`, `:408-409`. `Node.h` `:64-68, 1319-1327, 1349, 1388, 1440`, signal `outputLayerChanged` `:1456,1512` → `layerSelectionChanged`. `NodeInputs.cpp:338-341, 1624`. Viewer (`Gui/ViewerTab40.cpp:857-995`, `ViewerTabPrivate.cpp:374-391`) only uses `getAvailableLayers` — no dependency. `Gui/NodeGui.cpp:3190-3230` `onOutputLayerChanged` → summary label; keep multiplanar branch (`Node.cpp:7526`). Premult warning (`Node.cpp:2691-2706`, `:7545-7600`) moved to index 1, re-based on Color row (M43 deletes it). Python: `getParam("channels")` ChoiceParam, `processAllLayers`, `<Input>_channels`, host `NatronOfxParamProcess*` go away; `Effect.addUserLayer` (`Engine/PyNode.cpp:989-1009`) stays. PyPlugs: `Gui/Resources/PyPlugs/{AngleBlur,DropShadow,EdgeBlur,Fill,Glow,LightWrap,PIKColor}.py` set `NatronOfxParamProcess*` (`EdgeBlur.py:482-483` aliases raise); `ZRemap.py:182-324`, `ZMask.py:241-326` use the implicit shuffle — USER DECIDED: drop them until M34. Tests: `Tests/WriteAllLayers_Test.cpp:158,232` use `kNodeParamProcessAllLayers` for the writer plugin's own param — replace macro with literal.

## A.4 GUI
`Gui/KnobGuiLayerChannels.{h,cpp}` derived from `KnobGui` (not KnobGuiTable), registered `Gui/KnobGuiFactory.cpp:106`; container QWidget/QVBoxLayout (`Gui/KnobGuiTable.cpp:201-207`); label column "Channels". Row = dropdown `None | All | Regex… | ─── | Color | diffuse | …` (Natron ComboBox) then Layer mode: checkable `Button` per channel, coloured per `Gui/KnobGuiBool.cpp:280-298`; Regex mode: LineEdit, validate on editingFinished, red border + errorString tooltip, "matches: …" label. Rows ≥2 get [−]; one [+ Add layer] under last row. Undo: `pushUndoCommand(new KnobUndoCommand<std::string>(…))` (`KnobGui.h:136`) with new `setMergeable(false)` (`Gui/KnobUndoCommand.h:85`, `mergeWith` `:289-306`). `updateGUI` rebuilds from rows; slot on `availableLayersChanged()` (cf. `Gui/KnobGuiChoice.cpp:214`). Placement: `mainPage->insertKnob(0, knob)` in `initializeDefaultKnobs` (`Node.cpp:2785-2795`); `advancedSep` (`:2842-2867`) needs no change (first set by `createMaskSelectors` `:2555-2557`). "New" entry dropped from this widget (M36 re-adds as sentinel → `NewLayerDialog` + `addUserComponents`); keep `setHostCanAddOptions` for multiplanar OFX `outputChannels`.
Mockup:
```
Channels  [ Color   ▾ ] [R] [G] [B] [A]
          [ diffuse ▾ ] [R] [G] [B]                        [−]
          [ Regex…  ▾ ] [ spec.*     ] matches: specular   [−]
          [ + Add layer ]
Size      [ 3.00 ] …
──────────────────────────────────────  ← advancedSep
Mask      ☑ [ Mask.A ▾ ]  □ Invert Mask
Mix       [ 1.00 ]
```

## A.5 Compat
Old .ntp: `channels` Choice skipped by type-mismatch guard (`Node.cpp:1625-1628`); others ignored (`loadKnobs` `:1486-1490, 1602-1618`). USER DECIDED: clean break, no mapping. `NATRON_CACHE_VERSION` 5→6 (`Global/GlobalDefines.h:97`).

## User decisions so far (2026-09-19)
- Clean break for old projects. Regex: labels, anchored, case-sensitive, QRegularExpression. ZRemap/ZMask: drop until M34. One undo step per committed edit.
- Adopt the 28 plugins' RGBA into the widget AND put Merge on a widget variant with no add-rows and no regex. Goal: one consistent channel-selection UI across essentially all nodes, with small variations for specific purposes. Take time; get it right.

## v1 task list (to be revised): 38.1 harness+engine knob (T1 harness, T2 KnobLayerChannels+tests, T3 Node creation/API, T4 eligibility/defaults, T5 Python LayerChannelsParam); 38.2 render (T1 componentsNeeded, T2 per-plane fetch+masking, T3 identity, T4 regression guards); 38.3 GUI (T1 widget rows/undo, T2 channel buttons+regex editor, T3 NodeGui summary+premult rewire); 38.4 deletions (T1 Node cleanup, T2 cache bump+dump assert, T3 PyPlugs, T4 ZRemap/ZMask); 38.5 openfx-io Read label (fetch-assets.sh:222-223 OPENFX_IO_REF) + docs; 38.6 after-shots + user checkpoint on release AppImage.
