# Design: M34, the native Shuffle node (v1, approved 2026-09-22 with the answers in §3)

Governed by `2026-09-19-layer-registry.md` (§3 listing roles) and `2026-09-19-layer-channel-widget.md`. From the widget doc:
- §1: the knob types.
- §5: the no-shuffle invariant. Only Shuffle ever produces a plane whose channel list differs from its source.

Settled elsewhere and not re-argued here:
- **Clean break.** Old projects are not required to load (`DECISIONS/2026-09-18-clean-break-no-project-compat.md`). This overrides the stub's compatibility line.
- **Knob rules carried over:** `KnobTable` with untranslated tags, one undo step per action, no expressions on tables, and a cap of 4 channels per layer.

## 0. Findings

### The current Shuffle
The current Shuffle is the OFX plugin `net.sf.openfx.ShufflePlugin`, from openfx-misc `Shuffle/Shuffle.cpp` (2099 lines, v2 and v3 factories).

Host special cases:
- `PLUGINID_OFX_SHUFFLE` (`Engine/EffectInstance.h:72`).
- A v<3 "A is main" branch, which appears four times: `NodeInputs.cpp:1437,1913` and `EffectInstance.cpp:5035,5123`.
- `outputR..A` rename filters (`KnobSerialization.cpp:640-656`). These are dead under the clean break.

Where it is used:
- 12 instances across six PyPlugs:
  - DropShadow
  - EdgeBlur
  - Fill
  - Glow
  - LightWrap
  - PIKColor, whose `outputA` is driven by an expression and has to become a callback, as Glow's did.
- `Tests/LayerKnobs_Test.cpp:374`.
- ZRemap and ZMask were deleted "until M34" (`fd33e5ab8`).

### How M38 treats it
The plugin is multiplanar, so the host gives it no layer knob. It lists its own layers. The registry does not see its references (`Node::getReferencedLayerIDs` only walks host knobs), so the Layers page can remove a layer that a Shuffle still uses.

### Problems
1. **The A/B convention is inverted.** B has been the default since v3, but menus list A first, and the host carries the v2 inversion four times.
2. **The output channel count is split across three places:** four fixed `outputR..A` choices that are relabelled or hidden, an `outputComponents` knob that applies only to Color, and otherwise B's count, used silently.
3. **Stored values are opaque OFX plane IDs.**
4. **A missing input plane fails the render** with "Cannot find requested channels in input".
5. **Suspected: Color is allocated but never filled** when the target is another layer. The host forces the metadata layer into a multiplanar effect's produced planes (`EffectInstance.cpp:4487-4507`), and the plugin writes only `outputLayer`.
6. **Minor defects:** a cache key mismatch, a hard error on an A/B bit-depth mismatch, and opaque identity heuristics.
7. **One output layer per node.** A swap takes three nodes.

### Native-node precedent
- **Framework:** `NativeEffectBase` (`Engine/Nodes/README.md`), registered with `registerBuiltInPlugin<T>` (`AppManager.cpp:1546-1570`). Sources are globbed.
- **Two inputs:** DeepMerge.
- **Multiplanar output:** no native flat multiplanar node exists yet. The flat precedent is RotoPaint: it overrides `render()` over `args.outputLayers` and fetches with `getImage(inputNb, …, &layer, …)`, and it declares its planes with `getComponentsNeededAndProduced`.

### Gaps in M38 that M34 closes
- **(a)** Plugin-owned layer knobs get no listing role and are not counted as references.
- **(b)** `KnobLayerSelect` has no None.
- **(c)** A node cannot opt out of implicitly producing the metadata layer.

## 1. Semantics

### Identity and inputs
`fr.natron.Shuffle` (`Engine/Nodes/Channel/Shuffle.{h,cpp}`), in the Channel group. Image only, float only, with no mask, mix or premult.

Two optional inputs, **B** (input 0: preferred, pass-through) and **A** (input 1):
- Format, pixel aspect ratio and frame range come from B, or from A when B is not connected.
- The region of definition is B's, united with A's when A is read.

### Knobs

| script name | type | listing role | default |
|---|---|---|---|
| `in1Input`, `in2Input` | `KnobChoice` {B, A} | — | B, A |
| `in1`, `in2` | `KnobLayerSelect`, no buttons, allows None | input-bound to the chosen input | Color, None |
| `out1`, `out2` | `KnobLayerSelect`, no buttons; `out2` allows None | target (registry plus "New layer…") | Color, None |
| `mapping` | new `KnobShuffleMap` (`KnobTable`) | — | empty (= keep) |

### Mapping
Each output channel `outK.c` has one source: `inJ.c'`, `0`, `1` or **keep**.
- **Keep** means B's same channel of the same layer, or 0 when B lacks it. This is M38's in-place rule.
- **Unreadable sources** — slot None, layer absent, index out of range — behave as keep. A render never fails.
- **Storage is by slot and channel index**, not by layer name. Changing a layer never rewrites `mapping`, and "straight" wiring survives layer changes.
- **A new node** is a true identity.

### Planes and the registry
- **Produced planes:** `out1` ∪ `out2`. Color is always produced as RGBA.
- **Duplicate output:** `out2 == out1` resolves to None and is marked "(same as out1)".
- **Needed planes:** the slot layers per input, plus B's copies of the output layers.
- **Pass-through:** everything not produced passes through from B.
- **Metadata layer:** Shuffle opts out of producing it (gap c), so Color passes through untouched unless it is an output.
- **Registry references:** `in1`, `in2`, `out1` and `out2` count as references (gap a).

### Identity, label, Python and serialization
- **`isIdentity`:** B, when `out2` is None and every `out1` channel is keep or straight from `in1`, with `in1` on B and the same layer as `out1`.
- **Sub-label:** `(diffuse → Color)`.
- **Python:** `LayerSelectParam` for the four layer knobs, plus a new `ShuffleMapParam`:
  - `connect("in2.Z","out1.A")` also accepts `"0"` and `"1"` as the source.
  - `disconnect`, `getSource`, `getConnections`, `reset`.
  - Unknown names raise `ValueError`.
  - The exporter emits `connect()` calls.
- **Serialization:**
  - `mapping` rows look like `<Out>out1.3</Out><Src>in2.0</Src>`.
  - New typeName `ShuffleMap`.
  - No version bump.

### The old OFX Shuffle
Hidden as internal-only, like the OFX Roto (`OfxHost.cpp:1034-1037`). It can still be created by ID. The host special cases are deleted and the PyPlugs are ported.

## 2. UI
Three options were considered:
- (a) Per-output dropdowns: today's UI, which the user called counter-intuitive.
- (b) Shuffle2-style noodles: a costly custom canvas.
- **(c) A toggle-button matrix (recommended).**

How the matrix works:
- One row per output channel. Each row is an exclusive radio group, with columns for the in1 channels, the in2 channels, keep, 0 and 1.
- One click is one value and one undo step.
- It is built from `Button`, with the R/G/B/A colours.
- The worst case is 8×11 buttons.
- `out2` rows appear only when `out2` is set.
- An absent slot layer greys out its columns and shows the "(not in input)" marker.

```
Input 1   [ B ▾ ] [ diffuse ▾ ]
Input 2   [ A ▾ ] [ depth   ▾ ]
Output 1  [ Color ▾ ]
Output 2  [ None  ▾ ]
                   in1 diffuse    in2 depth
                    R   G   B        Z        keep   0   1
Color  R   ←       [■] [ ] [ ]      [ ]       [ ]   [ ] [ ]
       G   ←       [ ] [■] [ ]      [ ]       [ ]   [ ] [ ]
       B   ←       [ ] [ ] [■]      [ ]       [ ]   [ ] [ ]
       A   ←       [ ] [ ] [ ]      [■]       [ ]   [ ] [ ]
          [ Reset ]
```

## 3. Open questions (the consultant's recommendation is first in each)
1. **Output slots:** two (like Shuffle2, so a swap is one node), or one?
2. **Output channel with no readable source:** keep B's value (the in-place rule, degrades visibly), or black?
3. **Old OFX Shuffle:** hide it as internal-only, delete it from the fork build, or keep it visible?
4. **Read of a file with no RGBA** (the first layer is duplicated into Color): leave it, or fix it here?

### Answers (user, 2026-09-22)

1. **Two output slots.**
2. **Keep, but error if missing.** An unwired output channel keeps B's value. A wired source that should be readable but isn't fails the render with a persistent error naming the channel. This mirrors M38's mask rule.
   - Readable-but-isn't: its slot's input is connected, but that input lacks the slot's layer, or the channel index is beyond the layer.
   - Silent cases, where the wired channel behaves as keep: a disconnected input, or a slot set to None.
   - This amends §1's "unreadable sources behave as keep".
3. **Delete the OFX Shuffle from the openfx-misc fork build.** A fork PR drops `Shuffle` from the build, and `OPENFX_MISC_REF` is bumped. Graphs that contain the OFX Shuffle no longer load it, which the clean-break decision accepts. The host special cases are deleted as planned.
4. **Not asked; default taken.** The no-RGBA Read residual is left alone.
5. **UI: the toggle matrix.**
