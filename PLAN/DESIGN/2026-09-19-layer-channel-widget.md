# Design: the three layer/channel knobs (M38, Phases 38.2+)

2026-09-19, v3. Replaces v2 (whose "one knob with variant flags" model and Merge section are rejected — user review round 1, `M38-channel-layer-ui-organization.md` Decisions). Governed by `2026-09-19-layer-registry.md` (approved; §3 listing rules apply verbatim). v1's engine/render scouting (annex A.2/A.3 of v2) is carried over here with citations re-verified on this checkout; where a v2 citation was stale it is corrected in place and noted. Decided elsewhere and not re-argued: clean break for old projects (`docs/decisions/2026-09-18-clean-break-no-project-compat.md`); regex on layer labels, whole-string anchored, case-sensitive, `QRegularExpression`; ZRemap/ZMask dropped until M34; one undo step per user action; `NATRON_CACHE_VERSION` 5→6.

## 0. Goals / non-goals

**Goals.**
1. Three knob types, chosen by the *shape of the value* a node needs, not by flags: a **channel set** (many rows), a **layer select** (one layer, optional channel buttons), a **channel select** (one `layer.channel`).
2. One meaning for a channel row everywhere: "this node processes these channels of this layer, in place". Unselected channels are copied from the preferred input's *same* plane; unselected planes pass through. No node other than Shuffle moves data between layers or channels.
3. One GUI row (`LayerChannelRow`) shared by the three `KnobGui*` classes, so Blur, Roto, Tracker, a mask footer and Write look and behave the same.
4. Plugin adoption without plugin changes for the openfx-misc nodes that declare the standard `NatronOfxParamProcessR/G/B/A` quad: their defaults seed the Color row, the bools go secret and are forced true, the host masks.
5. The premultiplied/unpremultiplied concept leaves the app (M43 folded in as Phase 38.2) *before* any knob is built, so nothing in this design is premult-aware.
6. "New layer…" on every target knob (M36 folded in as Phase 38.7).

**Non-goals.** Merge (native node, own milestone — §11 has the stub note; the OFX Merge is an ordinary node and may break, accepted). Shuffle (M34; untouched here). Deep nodes (M60; they get no knob). More than 4 channels per layer (registry doc §1.1, `kLayerMaxChannels`). Any project migration.

## 1. The three knob types

All three derive from `KnobTable` (`Engine/KnobTypes.h:1079-1140`), which already gives a single-string value per dimension, `canAnimate() == false` hard-wired (`:1136-1139`), and the `<Tag>cell</Tag>` codec (`Engine/KnobTypes.cpp:2375-2461` decode, `:2478-2495` encode, cells XML-escaped by `Project::escapeXML` `Engine/Project.cpp:2383-2442`). The two singles are one-row tables rather than "plain string knobs with a documented format" (the alternative the user left open): one codec, one escaping path, one `createDuplicateOnHolder` branch (§1.6) and one `KnobSerialization::createKnob` case each, instead of a second ad-hoc parser for the same data. They are also *not* `KnobChoice`s: a choice persists an index plus a label and re-derives the value from its current entries, whereas a selected-but-absent layer must persist as an ID independent of whatever list is showing (registry doc §1.2, §3).

Three distinct `typeName()`s — `ChannelSet`, `LayerSelect`, `ChannelSelect` — so the load-time type-mismatch guard (`Engine/Node.cpp:1624-1627`) keeps them apart from each other and from the old `Choice`/`Bool` knobs (clean break: old values are simply skipped).

**Column tags are untranslated ASCII literals.** `KnobLayers` tags its cells with `tr()`-ed column labels (`Engine/KnobTypes.h:1174-1183`), so its on-disk format is locale-dependent. The new knobs override `getColumnLabel` to return fixed tags (`Mode`, `Layer`, `Channels`, `Channel`); display labels are a separate accessor used only by the GUI.

### 1.1 `KnobChannelSet` — script name `channels`, label "Channels"

Rows, three columns:

| tag | content |
|---|---|
| `Mode` | `none` \| `all` \| `layer` \| `regex` — `none`/`all` legal on row 0 only |
| `Layer` | layer ID (`ImageLayerDesc::getLayerID()`, `Engine/ImageLayerDesc.h:110`; Color is `kNatronColorLayerID` = `kFnOfxImagePlaneColour`, `:55`) for `layer` rows; the pattern verbatim for `regex` rows; empty otherwise |
| `Channels` | comma-joined enabled channel *names* for `layer` rows (empty = none enabled); empty otherwise |

`.ntp` example (one string, no row delimiter — rows are recovered by cycling the three tags, exactly as `decodeFromKnobTableFormat` does today):

```
<Mode>layer</Mode><Layer>uk.co.thefoundry.OfxImagePlaneColour</Layer><Channels>R,G,B</Channels><Mode>regex</Mode><Layer>spec.*</Layer><Channels></Channels>
```

C++ API (sketch):

```cpp
struct ChannelSetRow { enum Mode { eNone, eAll, eLayer, eRegex } mode; std::string layerOrPattern; std::vector<std::string> channels; };
struct ResolvedLayer { ImageLayerDesc desc; std::bitset<4> channels; };
class KnobChannelSet : public KnobTable {
    std::vector<ChannelSetRow> getRows() const;                 // decoded, cached under mutex, rebuilt on value change
    void setRows(const std::vector<ChannelSetRow>&, ValueChangedReasonEnum);
    // convenience, each = one setValue (one undo step):
    void setNone(); void setAll();                              // row 0 only, by definition
    void setLayer(int row, const std::string& id, const std::vector<std::string>* channelsOrAll);
    void setChannels(int row, const std::vector<std::string>&);
    void setRegex(int row, const std::string& pattern);
    int  addLayer(const std::string& id, const std::vector<std::string>* channelsOrAll);
    int  addRegex(const std::string& pattern);
    void removeRow(int row);                                    // row >= 1
    std::vector<ResolvedLayer> resolve(const std::list<ImageLayerDesc>& present) const;   // pure
    bool isPatternValid(int row, QString* error) const;         // compiled QRegularExpression cached per row
    std::string getSummary() const;                             // "Color.rgb", "All", "/spec.*/", "None", joined by ", "
};
```

`resolve(present)` is a **pure function of (rows, present list)** — the knob does not own the list, so the same function is called by the engine (`getComponentsNeededDefault`, §5) and by tests with a hand-built list. Semantics, in order:

1. Row 0 `none` → empty result. Row 0 `all` → every present layer, every channel. In both cases rows 1+ are ignored.
2. `layer` row → the present layer with the same ID (ID match, not `operator==`, because the registry may know a different channel *count* than the stream, registry doc §1.3); channel bits = enabled names ∩ the present layer's actual channel names; absent ID → contributes nothing.
3. `regex` row → every present layer whose *label* matches `QRegularExpression::anchoredPattern(p)`, case-sensitive, all channels; invalid pattern → nothing.
4. Same ID from several rows → one entry, channel sets OR-ed.
5. Bit mapping: channel index c → bit c, except a 1-channel layer → bit 3 (`Image::canCallCopyUnProcessedChannels`, `Engine/ImageCopyChannels.cpp:567-585`: "1 component is alpha").
6. Ordering: Color first if present, then row order. (The premult reason v1 gave for Color-first — `outputComponents.front()` — disappears with 38.2; the rule is kept so that `outputLayerBeingRendered` and the plane loop are deterministic for non-multiplanar plugins, `Engine/EffectInstance.cpp:2618-2621`.)
7. A layer row whose channel set is empty contributes nothing (the row is kept; the GUI shows all buttons off).

**Default row 0**: Color with the plugin's default set — the `kNatronOfxImageEffectPropChannelSelector` value (`Engine/OfxEffectInstance.cpp:3068-3090`) or, when the plugin declares its own quad, the quad's defaults (Grade: A off, `Grade/Grade.cpp:1347`); native nodes RGBA (`Engine/EffectInstance.h:1860-1871`). Write: Color, all channels (§4).

### 1.2 `KnobLayerSelect` — script name `layer`, label "Layer"

One row, two columns: `<Layer>id</Layer><Channels>R,G,B,A</Channels>`. `Channels` is empty on knobs created without buttons (Tracker) and means "all". API: `getLayer/setLayer(id)` (resets channels to all of the layer's channels — the shared rule), `getChannels/setChannels`, `resolve(list) → optional<ResolvedLayer>` (same ID-match/intersection/bit rules as one `layer` row). Creation flag `withChannelButtons`.

### 1.3 `KnobChannelSelect` — script names `maskChannel_<Label>` / `inputChannel_<Label>` (unchanged), label empty

One row, one column: `<Channel>layerID.C</Channel>`; empty cell = **none**. The ID format is the one `ImageLayerDesc::getChannelOption` already persists in mask choices (`Engine/ImageLayerDesc.cpp:249-269`: `<layerID>.<chan>`). API: `get/set(std::string)`, `resolve(list) → optional<pair<ImageLayerDesc,int channelIndex>>` (replacing `Node::getMaskChannel`'s ID comparison at `Engine/Node.cpp:7614-7637`). Default `Color.A` (today's `setDefaultValue(4)`, `Node.cpp:2536-2537`).

### 1.4 Shared: population source and roles

Every knob of the three types carries a `LayerSource { int inputNb; enum Role { eInputBound, eTarget }; }` set by the node at creation (not serialized — it is a property of the node kind, re-derived on every creation, so a `.ntp` cannot carry a stale role). The knob is pure; **listing is a `Node` service**: `Node::listLayersForKnob(const KnobIPtr&, std::list<ImageLayerDesc>*)`:

- `eInputBound`: `getPresentLayers(time, view, inputNb)` (registry doc §3, added in 38.1.T3). `inputNb == kPreferredInput` (the channel set's case) resolves to `getPreferredInput()` at call time (`Engine/NodeInputs.cpp:1404-1455`). **Color is always listed** on input-bound knobs — every image stream carries a Color plane (metadata comps default to RGBA, `Engine/EffectInstance.cpp:4270-4273`) — so an unconnected Blur lists exactly `Color` instead of an empty menu. No "New layer…".
- `eTarget`: the registry snapshot, registry order, built-ins first, plus the "New layer…" sentinel (§7).
- Aliases (`getAliasMaster()`, `Engine/Knob.h:1235-1250`) delegate to the master's node: the group node has no layer list of its own.

"Present" per role: input-bound = produced ∪ pass-through of the input stream, as `getPresentLayers` defines it; target = irrelevant — a target knob's node *creates* the layer in its output (`EffectInstanceRenderRoI.cpp:492-499` already treats a produced plane that way).

**Marker for a selected-but-absent value.** The knob keeps the ID. The GUI shows the *current value* as one extra combo item `diffuse (not in input)` (input-bound) or `diffuse (not in project)` (target), inserted only while it is the current value and removed the moment another entry is chosen. The list itself never carries greyed entries (registry doc §3). `resolve()` returns nothing for it. Load of a hand-edited `.ntp` with an unknown ID: same behaviour, no dialog (registry doc §1.2).

**No animation, no expressions.** `KnobTable::canAnimate()` is already false. There is no per-knob expression refusal hook (`Engine/Knob.cpp:2825-2889`, the only guard is `validateExpression`), so add `virtual bool KnobI::supportsExpressions() const { return true; }`, false on `KnobTable`, checked at the top of `KnobHelper::setExpressionInternal` (throws `std::invalid_argument`) and by the `KnobGui` right-click menu (no "Set expression…" entry). Linking/aliasing stays allowed.

**Undo.** Every user action — button toggle, dropdown change (which also resets that row's channels), regex commit on `editingFinished`, add row, remove row, "New layer…" selection — is one `setValue` of one string, pushed as `KnobUndoCommand<std::string>` (`Gui/KnobUndoCommand.h:63-320`) with a new `setMergeable(false)` (today `_merge` is only cleared when a keyframe is added, `:250-253`; `mergeWith` `:289-306`). Because a whole set is one value, the v2 "macro across knobs" problem is gone — nothing else changes on a dropdown reset. Registry additions made by "New layer…" are not undoable (registry doc §2), only the knob change is.

**Aliasing in PyPlug groups.** `Knob::createDuplicateOnHolder` has a hand-written type switch with no table branch — it returns null for any `KnobTable` (`Engine/Knob.cpp:4525-4667`). Add one branch covering the three types (copy the `LayerSource` and `withChannelButtons` flag); `setKnobAsAliasOfThis` already gates on `typeName()` and dimension (`:4762-4768`). Python `Param.setAsAlias` (`Engine/PyParameter.cpp:428-441`) then works unchanged. `Effect.createChannelSetParam/createLayerSelectParam/createChannelSelectParam(name, label)` exist for the PyPlug exporter's user-knob emission.

### 1.5 Python API (per type, no "raises if variant" cases)

```python
p = node.getParam("channels")            # ChannelSetParam
p.getRows()                              # [("layer","Color",["R","G","B"]), ("regex","spec.*",[]), ("all","",[]), ("none","",[])]
p.setNone(); p.setAll()                  # row 0, by definition
p.setLayer("diffuse", channels=None, row=0)   # None => all channels of that layer; resets channels on layer change
p.setChannels(["R","G"], row=0)
p.setRegex("spec.*", row=0)              # added to the user's list: row 0 may be a regex too
p.addLayer("specular", channels=None)    # -> new row index
p.addRegex("spec.*")                     # -> new row index
p.removeRow(i)                           # i >= 1
p.resolve()                              # [("Color",["R","G","B"]), ...] against the node's present list now

q = node.getParam("layer")               # LayerSelectParam
q.getLayer(); q.setLayer("diffuse"); q.getChannels(); q.setChannels(["R"])

m = node.getParam("maskChannel_Mask")    # ChannelSelectParam
m.get()                                  # "Color.A" or "" (none)
m.set("diffuse.R"); m.set("")
```

`ValueError` only for: `removeRow(0)`, a row index out of range, `none`/`all` requested on a row ≥ 1, `setChannels` on a layer select created without buttons, and a channel name the *registry* says the layer does not have (unregistered IDs are accepted as typed — the registry doc's "never invent a layer" rule cuts both ways). An invalid regex is accepted and stored (it resolves to nothing; the GUI shows it red) — Python and GUI agree. `getParam` wrapper factory: `Engine/PyNode.cpp:390-456` gains three branches; wrapper classes in `Engine/PyParameter.h` and `Engine/typesystem_engine.xml:1389-1404` (`PathParam` shows the list-out-param idiom, `:1404ff`); shiboken runs at build time from `Engine/CMakeLists.txt:52-70` — nothing checked in to regenerate, but the new header must be included from `Engine/PySide6_Engine_Python.h`.

### 1.6 Registration touch points

`Engine/KnobFactory.cpp:79-96` (`loadBultinKnobs`), `Engine/KnobSerialization.cpp:143-185` (second hand-written switch — a missing case means the project silently drops the knob), `Gui/KnobGuiFactory.cpp:90-107`, `Engine/Knob.cpp:4525-4667` (alias branch), `Engine/PyNode.cpp:390-456`.

## 2. The shared GUI row

One widget, `Gui/LayerChannelRow.{h,cpp}`: `[ComboBox] [buttons…] | [LineEdit pattern] [matches: …] [−]`, with a mode enum `{eSetRow0, eSetRowN, eLayer, eChannel}` that only changes which entries the combo offers and whether buttons/`[−]` exist. Three thin `KnobGui` classes (`KnobGuiChannelSet`, `KnobGuiLayerSelect`, `KnobGuiChannelSelect`) derive from `KnobGui` (not `KnobGuiTable`) and put one composite `QWidget` with a `QVBoxLayout` in the field column, the precedent being `KnobGuiTable::createWidget` (`Gui/KnobGuiTable.cpp:206-281`; `shouldAddStretch()` false, `Gui/KnobGuiTable.h:109`). The label column stays a single left cell (`Gui/KnobGuiContainerHelper.cpp:849-861`), and same-line placement for the mask footer keeps working through `setAddNewLine(false)` (`:404-421`, `:678-698`).

- Combo entries, in order. Set row 0: `None`, `All`, `Regex…`, separator, `Color`, other layers (list order). Set rows 1+: `Regex…`, separator, layers. Layer select: layers (target: registry order + separator + `New layer…`). Channel select: `None`, then every `layer.channel` of the present layers. Natron `ComboBox` has `addSeparator`/`addItemNew`/`setCurrentIndex_no_emit` (`Gui/ComboBox.h:113-199`).
- Buttons: one checkable `Button` (`Gui/Button.h:41-57`, `QPushButton::setCheckable`) per channel of the row's layer, text = channel name, tooltip = `layer.channel`, coloured by *name* with the constants from `Gui/KnobGuiBool.cpp:273-300` (R/G/B/A; other names neutral). Hidden in `none`/`all`/`regex` modes.
- Regex editor: `LineEdit`; validate on `editingFinished`; invalid → red border, tooltip = `QRegularExpression::errorString()`; a `matches: diffuse, specular` label from the current list. Enter commits, Esc reverts to the last committed pattern.
- Row 0 on `None`/`All` greys rows 1+ (values kept). `[−]` on rows ≥ 1; `[+ Add layer]` under the last row (adds a `layer` row on the first listed non-Color layer, or Color if none).
- Choosing a layer resets that row's buttons to all-on (user decision (e)).
- Keyboard: dropdown → buttons left-to-right → `[−]`; next row; `[+ Add layer]` last; Space toggles a focused button.
- Disabled/read-only: the whole composite greys; the NodeGui summary still updates.
- Repopulation: `KnobGui` slots on the node's input-related refresh (the existing `refreshChannelSelectors` → `onChannelsSelectorRefreshed` path, `Engine/Node.cpp:7640-7711`, kept as the notification point) and on `Project::projectLayersChanged` (registry doc §5) for target knobs.

**Mockups.**

Blur (channel set, input-bound):
```
Channels  [ Color   ▾ ] [R] [G] [B] [A]
          [ diffuse ▾ ] [R] [G] [B]                              [−]
          [ Regex…  ▾ ] [ spec.*      ]  matches: specular       [−]
          [ + Add layer ]
Size      [ 3.00 ] …
──────────────────────────────────────────  ← advancedSep (unchanged)
Mask      ☑ [ Color.A ▾ ]  □ Invert Mask
Mix       [ 1.00 ]
```

Roto (layer select + buttons, target, registry-listed):
```
Layer     [ mask (not in project) ▾ ] [R] [G] [B] [A]     ← marker only while the value is absent
          ┌ Color · DisparityLeft · DisparityRight · Backward · Forward · depth · diffuse · specular ─ New layer… ┐
```

Tracker (layer select, no buttons, input-bound):
```
Layer     [ Color ▾ ]
```

Mask footer (channel select — row shape unchanged from today, only the knob type differs):
```
Mask      ☑ [ diffuse.R ▾ ]  □ Invert Mask
```

Write (channel set on the container):
```
Channels  [ All ▾ ]                      ← "All" = today's "All Layers"
          [ … rows greyed … ]
File      [ /out/comp.####.exr ]
```

**Placement.** The v2 citation `mainPage->insertKnob(0, …)` at `Node.cpp:2785-2795` is stale — no such call exists. Today the host bools are inserted by `findOrCreateChannelEnabled` at `mainPage->insertKnob(i, …)` (`Engine/Node.cpp:2662, 2685`) and the premult warning at index 4 (`:2704`), all from `initializeDefaultKnobs` (`:2726-2899`; the call at `:2794`). The new rule: the node's layer knob (`channels` or `layer`) is inserted at **main-page index 0** from the same spot; the mask footer is created by `createMaskSelectors` (`:2493-2561`, called at `:2813`) exactly as today with the choice replaced by a `KnobChannelSelect` of the same name; `advancedSep` (`:2842-2867`) is untouched.

## 3. Which node gets which knob

Eligibility becomes one virtual, `EffectInstance::getLayerKnobSpec() → {kind ∈ {eNone, eChannelSet, eLayerSelect}, role, withChannelButtons}`, replacing `getCreateChannelSelectorKnob` (`Engine/EffectInstance.cpp:4522-4526`, overrides `ReadNode.cpp:978`, `WriteNode.cpp:970`, `TrackerNode.h:119-122`, `RotoPaint.h:125`). Default: `!isMultiPlanar() && !isReader() && !isWriter() && !furnace && getOutputDataKind() == eDataKindImage` → `{eChannelSet, eInputBound, true}`; `isGenerator()` (`Engine/OfxEffectInstance.cpp:794-815`) → `{eLayerSelect, eTarget, true}`. Existing "no knobs" overrides (Dot, Group, Input/Output, Viewer, Backdrop) stay `eNone`. Mask/alpha-only inputs get a channel select regardless of kind (as today, `Node.cpp:2759-2777`).

| node | knob | role / list | buttons | notes |
|---|---|---|---|---|
| ordinary filters (Blur, Grade, Invert, ColorCorrect, CImg*, …) | channel set `channels` | input-bound, present | yes | standard quad adopted (§5); default Color with the quad's defaults |
| Transform-style, Switch, TimeOffset, NoOp, Dot-like plugins with `ChannelSelector = None` and no quad | channel set | input-bound | yes | user decision (e): full set even here |
| generators (Constant/Solid, Ramp, Rectangle, Radial, CheckerBoard, ColorBars, ColorWheel, Rand, Plasma, Shadertoy — `addSupportedContext(eContextGenerator)`) | layer select `layer` | **target, registry + New layer…** | yes | a generator writes into one layer; Ramp/Rectangle/Radial own quads (`Ramp.cpp:80-82, 842`; `Rectangle.cpp:105-107, 1281`; `Radial.cpp:96-98, 1013`) and Constant's host quad are adopted as the buttons' defaults |
| Read | **none** | — | — | `outputComponents`, `outputLayer`/`outputLayerChoice`, `filePremult`, `outputPremult` hidden host-side (§4) |
| Write (container) | channel set `channels` | input-bound (input 0) | yes | drives the embedded encoder's plane list (§4); default Color all channels |
| Tracker | layer select `layer` | input-bound (input 0 "Source") | **no** | replaces `trackRed/Green/Blue` (§4) |
| Roto / RotoPaint | layer select `layer` | **target, registry + New layer…** | yes | replaces the four `NatronOfxParamProcess*` bools (`Engine/RotoPaint.cpp:197-214`); defaults RotoPaint all-on, Roto A-only (`:118-145`) |
| Shuffle (OFX) | none (plugin-owned knobs) | — | — | M34 |
| OFX Merge | channel set (ordinary rule) | input-bound | yes | its `AChannels*/BChannels*/OutputChannels*` quads are *not* adopted (non-standard prefix) and stay visible; may break — accepted |
| multiplanar plugins (Premult, Unpremult, STMap, IDistort, LayerContactSheet) | none (plugin-owned `inputPlane` + own quad, as today) | — | — | a multiplanar plugin chooses its own planes (`Premult.cpp:120, 1055`; `Distortion.cpp:3474-3482, 3643`); the host adds nothing (`getCreateChannelSelectorKnob` is already false for them). Premult/Unpremult are **kept** (§6.4) |
| deep nodes (`outputKind == eDataKindDeep`) | none | — | — | removes DeepMerge's stray host rows (`build/m38scout/dump.txt:363-388`); M60 |
| Viewer | none (present-only toolbar combos, 38.1.T6) | — | — | unchanged |
| every mask / alpha-only input of any node | channel select `maskChannel_<Label>` / `inputChannel_<Label>` | input-bound (that input), present | — | `None` + `layer.channel` entries; default `Color.A` |
| PyPlug group aliases | same type as the master | delegated to master's node | as master | §1.4 |

## 4. Node-specific mechanics

**Read.** `outputComponents` is defined in `IOSupport/GenericReader.cpp:200-207` (RGBA/RGB/RG/Alpha, default RGBA, `:3205-3231`) and drives the output clip components at `:2330-2331`; but the plugin *also writes it itself* — `inputFileChanged` guesses from the first layer's channel count (`:1974-2082`, `setOutputComponents` at `:2069`; ReadOIIO's `guessParamsFromFilename` `OIIO/ReadOIIO.cpp:1763-1849`) and ReadOIIO sets it on `outputLayer` edits (`:703-727`). **Decision: host-side hide, no fork change.** The guess is exactly the behaviour we want (the Color plane follows the file: an RGB JPEG yields RGB, a lone-A file yields Alpha) and removing the param from the fork would need a replacement for `setClipComponents` anyway. `ReadNode` hides by name after `createReadNode` (`Engine/ReadNode.cpp:646-802`), following `refreshFileInfoVisibility` (`:806-823`): `outputComponents`, `outputLayer`, `outputLayerChoice` (the Read-side implicit shuffle: `outputLayer` routes any file layer into the Color plane, `ReadOIIO.cpp:2316-2327, 3007-3030`), plus `filePremult`/`outputPremult` (§6). `outputLayer` is pinned to the Color entry whenever the filename changes. All other file layers reach the stream as their own planes through `getClipComponents` (`:769-819`) and the registry auto-registers them (registry doc §2b). Residual: a file with *no* R/G/B/A channels — ReadOIIO's first-layer default puts that layer into Color; see §9 and open question 1. `OPENFX_IO_REF` (`tools/ci/local/fetch-assets.sh:222-223`) is not bumped by this phase.

**Write.** The encoder decides what to write from `getClipComponents` (`OIIO/WriteOIIO.cpp:573-598`): with `processAllLayers` (`SupportExt/ofxsMultiPlane.h:76-78`) it asks `_inputClip->getPlanesPresent()` (`:585-591`), i.e. `kFnOfxImageEffectPropComponentsPresent` on its source clip, which the host computes from `getAvailableLayers(inputNb)` (`Engine/OfxClipInstance.cpp:293-315`); otherwise a single plane from `outputChannels` (`:593-596`). The render then writes exactly `args.planes` (`GenericWriter.cpp:1033-1050`). **Decision: host feeds the plane list; no fork change.** The `WriteNode` container gets `KnobChannelSet channels` (input-bound, input 0). On the embedded encoder the host sets `processAllLayers` true + secret + non-persistent, and hides `outputChannels` ("Layer(s)") and GenericWriter's own colour remap `outputComponents` (`GenericWriter.cpp:148, 2484-2503`). A new virtual `EffectInstance::filterLayersForEmbeddedInput(inputNb, list*)` (no-op by default) is called at the end of `getAvailableLayers` / `getPresentLayers` when `getNode()->getIOContainer()` is set (`Engine/Node.cpp:4784`); `WriteNode` overrides it to intersect with `channels.resolve(present)`. So the encoder's "all planes present" *is* the user's selection, and `comps[-1]` of the embedded writer (`Engine/OfxEffectInstance.cpp:2876-2928`, rendered by `Engine/OutputSchedulerThread.cpp:2288-2308`) follows. Hash: the container's knob is folded into the embedded node's hash since M58 (`Engine/Node.cpp:809-814`, `:921-933`), so toggling the set re-renders. Channel subsets: for non-Color layers the filtered descriptor carries only the enabled channels and the input fetch extracts them from the full upstream plane (new `Image::extractChannels`, hooked where the embedded encoder's input plane is fetched, `Engine/EffectInstance.cpp:976-987`); for Color the plugin's colour path has fixed component counts, so the set maps to the smallest superset among RGBA/RGB/Alpha (`{A}`→Alpha, `{R,G,B}`→RGB, anything else with alpha → RGBA) by driving the hidden `outputComponents`. `Tests/WriteAllLayers_Test.cpp` stays meaningful: `:158-160, :183, :193, :232-234` switch from `kNodeParamProcessAllLayers` to `channels.setAll()` / `setLayer(Color)`; `expectRgbaOnly` (`:66-72`) becomes the Color-only case, `expectAllLayerPixels` (`:75-96`) the All case, and a new partial case (Color + diffuse) is added.

**Tracker.** The frame accessor always reads input 0 (`Engine/TrackerFrameAccessor.cpp:229`) and hard-codes the plane at `:352-353` (`ImageLayerDesc::getRGBComponents()`), then averages the enabled channels into libmv's MONO image (`:172-205`). A `KnobLayerSelect layer` (input-bound, input 0, no buttons) on the Tracking page replaces `trackRed/trackGreen/trackBlue` (`Engine/TrackerContextPrivate.h:103-111`, created `:216-243`, read `:1162-1165`): the accessor requests the resolved present layer and averages its channels (Color: R,G,B; an N-channel layer: all N). Absent layer → the accessor returns no image and tracking fails through its existing error path; the knob shows the marker.

**Roto / RotoPaint.** `getPreferredMetadata` (`Engine/RotoPaint.cpp:1349-1375`, `OVERRIDE FINAL`) advertises the selected layer (ID, channel count) instead of `4`/Color; `render` already forwards `args.outputLayers` to the bottom Merge (`:1479-1489, 1536-1550`); the internal tree's nodes (OFX Merge/Roto/Constant/Transform/Blur/Smear per `Engine/RotoDrawableItem.cpp:184-208, 261-268`) have their own host knobs set to the same layer by the RotoPaint on every layer change, so each writes plane L in place; `renderMaskInternal` picks cairo A8 or ARGB32 (`Engine/RotoContext.cpp:2877-2884`) and the conversion templates already dispatch on `srcNComps` 1..4 (`:2404-2430`), so 2- and 3-channel targets convert from ARGB32. The four bools (`:197-214`) and `copyChannels` (`:1528-1530`, applied via `copyUnProcessedChannels` at `:1649`) are replaced by the layer select's channel bits; the `premultiply` knob (`:217-227`, applied `:1491-1511`) goes in §6.

**Generators.** `getComponentsNeededDefault` with no inputs has an empty pass-through list and takes the output layer from the selector (`Engine/EffectInstance.cpp:4230-4240, 4298`); with the layer select, `comps[-1]` = the selected registry layer (mapped to the plugin's clip-pref count as today, `:4275-4278`), channel bits from the buttons; when a Source is connected (Ramp/Rectangle/Radial composite over it) the same plane is read from Source and unselected channels are host-copied.

## 5. Render model

Carried over from v1 A.2 (citations re-verified): today `getComponentsNeededDefault` (`Engine/EffectInstance.cpp:4219-4372`) sets `comps[-1]` from the Output Layer choice (`:4298-4313`) and `comps[i]` per input (`:4318-4371`); `renderRoI` renders `comps[-1]` and pass-throughs the rest (`Engine/EffectInstanceRenderRoI.cpp:478-531`); `clipGetImage` returns `neededComps[input].front()` whatever plane is being rendered (`Engine/OfxClipInstance.cpp:853-874`); host masking `copyUnProcessedChannels` (`EffectInstance.cpp:2795, 2872`) copies from `imgs[preferredInput].front()` (`:2256-2257`) with **one** bitset for every plane — that is the implicit shuffle.

New:

1. **`comps` from the knobs.** `comps[-1]` = `channels.resolve(getPresentLayers(preferredInput))` (Color row → metadata comps as today, `:4301-4313`; empty resolve → metadata comps, node identity per (5)); for a layer-select node, the one resolved layer; every non-mask `comps[i]` = the same list (no-shuffle invariant: plane L is read from every input); mask inputs from their channel select. `processChannels` becomes **per plane**: the ActionsCache entry (`Engine/EffectInstancePrivate.h:67-161`, fields `:70, 130-133`) stores `std::map<ImageLayerDesc, std::bitset<4>>`; the `processAllRequested` out-param and its dead consumer block (`EffectInstanceRenderRoI.cpp:457-476`) are deleted; callers `EffectInstanceRenderRoI.cpp:450`, `OutputSchedulerThread.cpp:2297`, `RotoSmear.cpp:243`, `EffectInstance.cpp:4476` updated.
2. **Per-plane input fetch.** `clipGetImage` picks the `neededComps[input]` entry equivalent (`ImageLayerDesc::findEquivalentLayer`, `Engine/ImageLayerDesc.h:200-226`) to `outputLayerBeingRendered` (`EffectInstance.cpp:2621`, `:4559`), falling back to front (masks, multiplanar); the analysis fallback at `OfxClipInstance.cpp:876-888` uses `listLayersForKnob` + `resolve`.
3. **Per-plane host masking — the one rule: the host masks for every node that owns channel buttons (channel set or layer-select-with-buttons); adopted plugin quads are forced true.** In `renderHandler`'s plane loop (`EffectInstance.cpp:2618`), `originalInputImage` and the bitset are chosen per plane (the equivalent plane of the preferred input, that plane's bits). `copyUnProcessedChannels` is already a plain copy (`NATRON_COPY_CHANNELS_UNPREMULT` undefined, `Engine/ImageCopyChannels.cpp:49, 121-126`), so per-plane masking changes no arithmetic. For the adopted openfx-misc plugins this moves masking from plugin to host — behaviour verified by 38.4.T4.
4. **Identity on empty.** `hasAtLeastOneChannelToProcess` (`Engine/Node.cpp:5846-5865`) = "resolve() non-empty and some bit set"; consumed at `EffectInstance.cpp:3758`. The identity "choice B" branch (`EffectInstanceRenderRoI.cpp:630-680`, keyed on `getChannelSelectorKnob(inputNbIdentity)`) is deleted: an identity node requests exactly the caller's planes upstream (choice A).
5. **No-shuffle invariant, stated once:** a node rendering plane L reads plane L from every non-mask input and writes plane L; channels of L it does not process are copied from the preferred input's plane L; planes not in its resolved set pass through untouched from the preferred input. Only Shuffle (M34) and Write's channel extraction (§4, confined to the encoder's input fetch) ever produce a plane whose channel list differs from its source.
6. `resolve()` runs inside `getComponentsNeededDefault`, cached per hash by the ActionsCache; the compiled regexes are cached on the knob; it is never on the per-tile path.

## 6. Premult removal (M43 as Phase 38.2)

**Scope.** `grep -rn -i premult Engine Gui` = 727 hits in 62 files; excluding `unpremult`/`premultiply` words, 570. Real concept files: ~41 in Engine, 5 in Gui (plus 7 PyPlugs and 4 `QImage::Format_ARGB32_Premultiplied` false positives). The concept's only *observable* in-app effects today are: the OFX clip/image property advertised to plugins, the viewer's GL blend mode and forced-alpha-1 texture path, the node-panel warning, and disk-cache `ImageParams` equality — everything else is plumbing. Two pieces are already dead: the host channel-copy arithmetic (`ImageCopyChannels.cpp:49`) and RotoPaint's premult-aware metadata (`RotoPaint.cpp:1354-1371`, commented out).

**Categorised touch points.**
- *(a) Metadata tracking*: `Global/Enums.h:412-417` (`ImagePremultiplicationEnum`); `Engine/NodeMetadata.cpp:52-53, 77, 95, 135, 187-195`, `.h:61-63`; `EffectInstance::getPremult` (`Engine/EffectInstance.h:950-952`, `.cpp:5564-5569`); derivation in `getDefaultMetadata` (`EffectInstance.cpp:5341-5342, 5359-5369, 5459-5462, 5481-5485`), forcing in `checkMetadata` (`:5747-5752`), warning refresh hook (`:5675`); `ImagePlanesToRender::inputPremult/outputPremult` (`EffectInstance.h:1613-1622`); `OfxEffectInstance::ofxGetOutputPremultiplication` (`.h:124`, `.cpp:2985-2987`, `:1341`); RenderStats (`RenderStats.h:94-95, 124`, `.cpp:78-79, 95, 136, 294-302, 386-397`; `OutputEffectInstance.cpp:479-488`); node info tooltip (`Node.cpp:2095-2109`).
- *(b) OFX plumbing*: `OfxClipInstance::getPremult` (`.h:101-106`, `.cpp:270-290`), converters (`.cpp:1255-1290`), per-image property (`:1490`); `OfxImageEffectInstance.cpp:1142, 1161, 1304, 1319, 1331-1347` (`updatePreferences_safe`); HostSupport `ofxhImageEffect.cpp:2586-2690, 2703, 2721, 2886`, `ofxhClip.cpp:250, 521-522, 558-559, 815, 864-865` (kept — ABI); Read/Write keep-lists naming `filePremult/outputPremult/inputPremult` (`ReadNode.cpp:90-91, 140-141`, `WriteNode.cpp:84, 126`).
- *(c) Render path*: `EffectInstanceRenderRoI.cpp:373` (capture), `:661-669` and `:1283-1287` (**the `outputComponents.front()` decisions**), `:1314-1327` (`inputPremult`), `:1384, 1452` (into `ImageParams`), `:1539`, `:1724, 1749` and `:165, 189-218` (`convertLayersFormatsIfNeeded` → `requiresUnpremult`, effective only for RGBA→RGB with a non-linear LUT, `Tests/Image_Test.cpp:261-272`); `EffectInstance.cpp:1050, 1133-1141` (`getImage`), `:2249-2263, 2350, 2373` (`renderHandler` args), `:2719-2726, 2756, 2830, 2862` (`unPremultRequired`), `:2795, 2872` (`copyUnProcessedChannels` args); `Image::_premult` and ctor args (`Image.h:194, 228, 239, 348, 1029`; `Image.cpp:682-755, 812-884, 980, 1274-1338, 1772-1775, 2089, 2312`), `Image::premultImage/unpremultImage` (`Image.h:824-841`, `Image.cpp:2414-2483`; only caller RotoPaint), `ImageConvert.cpp:243, 372-410, 471-527, 659, 678-910`, `ImageCopyChannels.cpp` template parameters (`:53-69, 203-223, 315-365, 374-390, 588-600`); `ImageParams.h:112, 125, 137-148, 211-218, 249, 265`; `EffectInstanceRenderDeep.cpp:623`; viewer `ViewerInstance.cpp:915, 1511, 1870, 1937, 2444-2508, 2800-2882, 2964`, `UpdateViewerParams.h:72, 121`, `ViewerInstancePrivate.h:77, 90, 106`, `OpenGLViewerI.h:127`; Roto `RotoContext.cpp:2547-2600, 2819`, `RotoSmear.cpp:162`, `RotoPaint.cpp:1491-1511, 1564, 1649-1651`; `Lut.h/.cpp` `bool premult` packing arg (separate concept, untouched).
- *(d) Knobs*: RotoPaint `premultiply` (`RotoPaint.cpp:217-227`, `RotoPaintInteract.h:399`); load filter for `premultChannel` (`KnobSerialization.cpp:619`); plugin params `premult`/`premultChanged`/`premultChannel` from `ofxsMaskMix.h:32-45, 60-90` on 30 plugins, `filePremult`/`outputPremult` (GenericReader `:176, 196, 2336-2357`), `inputPremult` (GenericWriter `:122-131, 2213, 2301`).
- *(e) Warning*: `NodePrivate.h:404`; creation `Node.cpp:2691-2706`; `checkForPremultWarningAndCheckboxes` `Node.h:1064`, `Node.cpp:7547-7611`; triggers `Node.cpp:5545`, `EffectInstance.cpp:5675`.
- *(f) Python*: `Effect::getPremult` (`PyNode.h:387`, `.cpp:1078-1087`); `typesystem_engine.xml:222`.
- *(g) Serialization*: `ImageParamsSerialization.h:112` (disk cache → cache version bump); fixtures `Tests/fixtures/{ocio-old-config,read-time-offset}.ntp` carry plugin premult knob values (harmless, generic knob load).
- *(h) Gui*: `ViewerGL.cpp:230-250, 381-460, 1484, 1507, 1524`, `ViewerGLPrivate.cpp:621-640`, `ViewerGL.h:184`, `ViewerGLPrivate.h:108, 128` (`BlendSetter`); `RenderStatsDialog.cpp:59, 376-402, 886, 954`.
- *(i) Tests*: `Image_Test.cpp:180, 218, 261-312`; `DeepPipeline_Test.cpp:262-267`; Deep tests passing the enum to `Image` ctors.

**Render-path change.** Both `front()` sites collapse: planes are allocated as-is (`allocateImagePlane` loses the enum), `convertLayersFormatsIfNeeded` is called with `requiresUnpremult = false` (already the effective value for every float/linear render), `ImagePlanesToRender` loses `inputPremult/outputPremult`, `renderHandler` and `copyUnProcessedChannels` lose their premult arguments, RGBA→RGB remaps simply drop alpha. `ImageParams` loses `_premult` → `NATRON_CACHE_VERSION` 5→6 (`Global/GlobalDefines.h:97`) — the single bump of this milestone; no later phase changes a cached struct.

**OFX boundary.** The property is ABI and stays answerable; the host answers a **constant `kOfxImageUnPreMultiplied`** for every clip and image. Why that constant: the Grade family auto-sets its `premult` box from the property on connect (`Grade/Grade.cpp:1188-1210`: only `kOfxImagePreMultiplied` turns it on), so "unpremultiplied" means *nothing happens automatically* — Nuke's default; `kOfxImageOpaque` would make `Premult` treat alpha as 1 (`Premult/Premult.cpp:644`). The plugin's clip-preferences answer is no longer read back (`OfxImageEffectInstance.cpp:1304`).

**Premult/Unpremult nodes are kept** — they are the tools by which a user takes the responsibility M43 hands over (Nuke ships both). But the pinned upstream plugin makes each an identity when the property already claims its output state (`Premult.cpp:756-770`), and zeroes its quad on Opaque (`:870-880`): no constant answer keeps *both* nodes working. **Decision: fork-and-fix openfx-misc** (the standing pattern, `tools/ci/local/fetch-assets.sh:152-154`; openfx-misc is currently upstream, `:227-235`): delete the two identity shortcuts and the `changedClip` auto-toggle in `Premult.cpp`, repoint `OPENFX_MISC_REPO/REF`. A native Premult/Unpremult pair is the long-term home (M37-era), not this milestone.

**What the user sees.** No premult knob or warning anywhere: the warning label is gone; RotoPaint's `premultiply` is gone; `filePremult`/`outputPremult` (Read), `inputPremult` (Write) and the 30 plugins' `premult`/`premultChanged`/`premultChannel` are hidden by name at knob creation (`OfxEffectInstance` after param creation; Read/Write containers after `createReadNode`/`createWriteNode`), left at their defaults (off); the node-info tooltip and RenderStats lose their premult lines; the viewer blends premultiplied-over-checkerboard always and forces alpha to 1 only for images that have no alpha channel (2- or 3-component), which is what the `opaque` texture template already means for those. Merge's alpha rule belongs to the future native Merge (§11). Premult removal runs **first** (Phase 38.2): the per-plane masking and `renderHandler` rewrite of 38.4 touches exactly the functions that carry premult arguments, so doing premult first means they are rewritten once, and the warning is created inside the channel-bool machinery that 38.8 deletes, so it must already be gone.

## 7. "New layer…" (M36 as Phase 38.7)

Target knobs (Roto/RotoPaint, generators; later native Shuffle/Merge) list the registry and end with a sentinel entry (`ComboBox::addItemNew`, `Gui/ComboBox.h:126`, id `__new_layer__`). Selecting it opens `NewLayerDialog` (`Gui/NewLayerDialog.cpp:245-318`, validating via `LayerRegistry::validate` since 38.1.T6) → `Project::addLayer(desc, eOriginUser)` → on success `setLayer(newID)` (one undo step for the knob; the registry add is not undoable, registry doc §2) → the combo repopulates through `projectLayersChanged`. On cancel or refusal (conflict message shown verbatim) the combo reverts to the previous value with no undo entry. Input-bound knobs never show the sentinel. The old `KnobGuiChoice::onItemNewSelected` path (`Gui/KnobGuiChoice.cpp:313-340`) dies with the choice knobs in 38.8.

## 8. Deletions, PyPlugs, Python surface

**Engine** (`Engine/Node.cpp`, corrected ranges): `setProcessChannelsValues` `:570-592` (re-implemented on the Color row for `Node::setProcessChannelsValues` callers), `createMaskSelectors` `:2493-2561` (keeps its shape, changes the knob type), `findOrCreateChannelEnabled` `:2633-2707` → `adoptChannelQuad`, `createChannelSelectors` `:2710-2723`, `createChannelSelector` `:2938-2991`, `getSelectedLayerChoiceRaw` `:5587-5599`, `onLayerChanged` `:5635-5678`, `refreshEnabledKnobsLabel` `:5681-5740`, `isPluginUsingHostChannelSelectors` `:5767-5771`, `getProcessChannel` `:5773-5785`, `getSelectedLayer` `:5788-5843`, `hasAtLeastOneChannelToProcess` `:5846-5865` (re-implemented), `refreshLayersChoiceSecretness` `:6494-6522`, `getChannelSelectorKnob` `:7519-7538`, `getProcessAllLayersKnob` `:7541-7544`, `getMaskChannel` `:7614-7637` (→ channel select `resolve`), choice loop of `refreshChannelSelectors` `:7640-7711` (function and `onChannelsSelectorRefreshed` kept as the viewer/GUI notification point), knob dispatch around `:5507-5550`; `NodePrivate.h:48-70` `ChannelSelector`, `MaskSelector::compsAvailable` (`:73-94`), `:404` warning, `:408-409`; `Node.h:59-70` names (keep `enableMask/enableInput/maskChannel/inputChannel/maskInvert`, drop `kOutputChannelsKnobName`, `kNodeParamProcessAllLayers`), `outputLayerChanged` (`:1456, 1512`) → `layerSelectionChanged`; `NodeInputs.cpp:338-341, 1624`; `getCreateChannelSelectorKnob` (`EffectInstance.cpp:4522-4526`, `.h:504-507`) → `getLayerKnobSpec`; `getAvailableLayers` keeps serving `kFnOfxImageEffectPropComponentsPresent` on output clips and `kNatronOfxExtraCreatedPlanes`. **Gui**: `NodeGui::onOutputLayerChanged` (`Gui/NodeGui.cpp:3186-3228`) → summary from `getSummary()` (`(All)`, `(diffuse)`, `(Color.rgb, /spec.*/)`), multiplanar branch kept. **Tests**: `WriteAllLayers_Test.cpp:158, 232` (§4).

**PyPlugs** (`Gui/Resources/PyPlugs/`, discovered by directory scan — no qrc/CMake list, `Engine/AppManager.cpp:1696-1711, 1952-2000`): `Fill.py:166-168, 234`, `AngleBlur.py:172`, `DropShadow.py:267-278`, `PIKColor.py:306-316`, `Glow.py:1014, 1208, 1426`, `LightWrap.py:1112, 1153, 1284-1286` rewrite `NatronOfxParamProcess*` writes to `getParam("channels").setChannels([...])`; `EdgeBlur.py:41-53, 471-479` rewrites its user bool + alias/expression onto the group's own channel set; the `premult`/`premultChanged` writes in `DropShadow.py:295-300`, `Glow.py`, `PIKColor.py`, `LightWrap.py` are removed (the params are hidden and off); `ZRemap.py` + `ZRemap.png` and `ZMask.py` (no icon exists) deleted; the exporter (`Engine/NodeGroup.cpp`) emits `app.addProjectLayer(...)` for every non-built-in layer referenced by any knob inside the group (registry doc §6).

**Python surface removed**: `getParam("channels")` as `ChoiceParam`, `processAllLayers`, `<Input>_channels`, host-created `NatronOfxParamProcess*` (plugin-declared ones remain reachable as secret `BooleanParam`s; writing them is pointless — forced true), `trackRed/Green/Blue`, RotoPaint `premultiply`, `Effect.getPremult`, the `ImagePremultiplicationEnum` type.

## 9. Risks

- **Render regressions from per-plane masking and moved masking ownership** for the adopted plugins: any plugin that reads `processX` to change its *algorithm* rather than to mask would change output once forced true. Guard: 38.4.T4 pixel tests on `Tests/fixtures/flat-three-layers.exr` (Grade, Invert, Blur, Multiply, Saturation, ColorCorrect) plus the AppImage checkpoint.
- **Premult removal is wide** (41 Engine files, 5 Gui) and mechanical; the risk is a silently changed pixel path, not a crash. Guards: `Image_Test`, `WriteAllLayers` pixel assertions (a red RGBA round trip), a viewer screenshot over checkerboard before/after.
- **Two plugin forks** are now load-bearing (openfx-io already; openfx-misc for `Premult.cpp`). Each is a few lines and a SHA in `fetch-assets.sh`, but it is a maintenance surface.
- **Read residual shuffle** for files without an R/G/B/A layer (ReadOIIO's first-layer default lands in Color) — open question 1.
- **Write Color subsets are supersets** (RGB/RGBA/Alpha), not exact channel lists; non-Color subsets are exact — open question 2.
- **Roto into 2/3-channel layers** rides cairo's ARGB32 path plus the existing conversion templates; untested territory, verified by a dedicated test in 38.6.T5.
- **Adoption by name is a convention**: a third-party plugin with a `NatronOfxParamProcessR..A` quad that means something else will be adopted. Documented in `ofxNatron.h`; no new OFX opt-out property (v2's was tied to the rejected variant model).
- **Third-party PyPlugs** scripting the old bools or `Effect.addUserLayer` break — clean-break policy.

## 10. Open questions (≤3)

1. **Read of a file with no R/G/B/A layer.** Today ReadOIIO's default `outputLayer` is the file's first layer, which lands in Color — the last implicit shuffle on the Read side. Fix in the openfx-io fork now (ReadOIIO emits an empty/black Color plane when the file has none) or accept the residual for M38? *Recommendation: accept for M38, file the fork fix as the first task of M34 (Shuffle) where the "route a layer into Color" workflow gets its proper home.*
2. **Write Color subsets.** Superset mapping through the encoder's RGBA/RGB/Alpha colour path (this doc) versus a GenericWriter fork change to write arbitrary Color channel subsets exactly. *Recommendation: superset in M38; exact subsets are rarely wanted for Color and the fork change touches every openfx-io writer.*
3. **Generator with Source connected and a non-Color target layer the Source does not carry** (Ramp over a Source that has no `mask` plane): unselected channels of the target have no source to copy from and are zero — or should the generator refuse (identity) until the layer exists upstream? *Recommendation: zero, as a produced plane is today (`EffectInstanceRenderRoI.cpp:502`); it is what "a writing node creates the layer in its output stream" means.*

## 11. Stub note: what the native Merge will need from these knobs

Two layer selects with channel buttons for A and B (input-bound to their own inputs, present-listed) and one for the output (target, registry-listed, "New layer…"); an "also merge" channel set (input-bound to B) for extra layers merged with the same operation; an alpha rule whose mask-side operand is a channel select (any `layer.channel`, `None`). Nothing in the three knob types needs to change for it; the plane-extraction helper from §4 (Write) is reusable for A/B channel subsets.

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
