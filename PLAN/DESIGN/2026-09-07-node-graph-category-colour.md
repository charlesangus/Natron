# Design: node graph category colour and user colour (M24, Phase 24.1)

2026-10-10. Implements M24.P1.T1. Scouted on `milestone/m24-node-graph-category-colour`;
cites are by file and line against that checkout. Every other M24 task (P2-P4) must
implement exactly the values pinned in §2-§8 without re-deriving them; §9 records
the two items that were open when this note was written and have since been
decided by the user, folded into the pinned spec rather than left open.

## 0. The problem this replaces

`NodeGui::getColorFromGrouping()` (`Gui/NodeGui.cpp:385-437`) picks one RGB triple —
reader/backdrop/writer/generator by effect flag, else the plugin's major
`PLUGIN_GROUP_*` string matched against ten literals, else `getDefaultNodeColor()` —
and that triple is the node's *entire* body colour (`applyBrush()`,
`Gui/NodeGui.cpp:1782-1792`, covering `_boundingBox`, `_nameFrame` and
`_resizeHandle`). The same chain is duplicated in `Gui/ProjectGui.cpp:304-351` to
guess, from a `> 0.05` RGB delta against the recomputed default, whether a loaded
project's saved colour was user-chosen. There is one colour channel; recolouring a
node for personal organisation and recolouring it as a category signal are the same
write, so the second always destroys the first. M18 Phase 18.4 already removed every
other kind signal at the node level (silhouette shape, tinted backdrop, input-arrow
glyphs), so colour is now the *only* place a node's kind can show at all, which is
what makes the single-channel design no longer tenable.

## 1. Survey: how four reference apps split "kind" from "user pick"

**Houdini** (the reference app for this milestone). A node's tile colour and its
*default* tile colour are the same attribute, `opcolor`, one per node instance. The
network editor's colour palette (Tools -> Show Palette: Colors, or `C`) drags a
swatch onto the selected node(s) and overwrites `opcolor` outright; holding Ctrl
while dragging instead calls `opdefaultcolor` and changes the *type's* default for
every future node of that type (nodes already placed keep whatever `opcolor` they
already have). So Houdini has exactly the problem M24 is fixing: colouring one node
and re-theming a whole category are the same slot, and the first overwrites
whatever the type default put there. What actually survives a user recolour in
Houdini is the node's **shape and icon** — every operator type ships its own glyph,
and SOP/VOP/DOP/LOP context nodes additionally differ in base tile shape — not a
second colour channel. Houdini ships a swappable "Default" colour theme (per-type
defaults) and a "No colors" theme, confirming the defaults are type-keyed exactly
like Natron's per-group `KnobColor`s, just without the border/body split.
[opcolor](https://www.sidefx.com/docs/houdini/commands/opcolor.html),
[Organizing, customizing and annotating nodes and networks](https://www.sidefx.com/docs/houdini/network/organize.html).

**Nuke.** Every node has one `tile_color` knob (a packed 32-bit RGBA int); the
class's shipped default is readable back via `nuke.defaultNodeColor(class)` but
there is no separate stored "this is the default" flag — setting `tile_color`
overwrites it, full stop, same single-slot pattern as Houdini. The signal that
survives a recolour is the fixed icon Nuke draws on every tile for that node class,
plus the class name in the tile label.
[Nuke Python: node colors](https://erwanleroy.com/nuke-python-making-your-first-functional-plugin-node-colors-presets-part-12/).

**Fusion.** Tool tiles have a `TileColor` a user sets per-tool (Tools menu / right
click) purely for personal organisation ("make it stand out so it's easy to find");
there is no evidence of a maintained per-category default colour scheme in stock
Fusion, and `REGS_Category` (a tool's registration category, e.g. "Filter",
"Color") only controls which Tools-menu submenu a tool appears under, not its tile
colour. Fusion does, however, already use colour for *state*, independent of the
tool's own colour: the active tool's tile flashes bright yellow and a
rectangle-selected group turns blue — a precedent for "a transient state colour
and a persistent identity colour are different things", even though Fusion's
identity colour is user-only, not category-driven.
[Fusion tool reference](https://documents.blackmagicdesign.com/UserManuals/Fusion9_Tool_Reference.pdf).

**Blender** (compositor / shader / geometry-nodes editors) comes closest to a
two-channel model, but it is still one slot. A node's header colour defaults from
its **colour tag** — a closed, built-in enum (`INPUT`, `OUTPUT`, `COLOR`,
`CONVERTER`, `DISTORT`, `FILTER`, `GEOMETRY`, `MATTE`, `SCRIPT`, `SHADER`,
`TEXTURE`, `VECTOR`, plus a few build-in-only tags) that is exactly the shape of
Natron's `PLUGIN_GROUP_*` → category mapping. A user can separately flip
`use_custom_color` and supply their own header RGB (and, since 4.x, a node *group*
can specify its own group-level header colour). But the custom colour **replaces**
the tag colour in the one header slot rather than sitting alongside it as a second
channel — recolouring a Blender node still erases which colour tag it carries
visually, it is only the underlying enum field that survives, invisibly.
[Node color tags / custom color](https://projects.blender.org/blender/blender/pulls/105102).

**Conclusion.** None of the four surveyed apps actually implements a persistent
category-colour-plus-user-colour split; all four either overwrite a single colour
slot (Houdini, Nuke, Fusion, Blender) or carry the surviving "kind" signal through
shape/icon instead of colour (Houdini, Nuke) — an option M18 already closed off for
Natron. The body/border split this milestone specifies is therefore new relative to
the prior art, not a port of an existing convention; §2-§8 below are chosen to fit
Natron's actual rendering primitives (`NodeGraphRectItem`, `_stateIndicator`), not to
match any surveyed app pixel-for-pixel.

## 2. The closed category list and default colours

`NodeCategoryEnum` (new, beside `DataKindEnum` at `Global/Enums.h:531`) has exactly
these values. Every existing per-group knob keeps its current `setName()` string
and current default (`Engine/Settings.cpp:922-1012` creation, `:1729-1777`
defaults) — P2.T1 must not change a single existing default. Two knobs are new
(Native 3D, USD 3D); their defaults below are chosen now, not placeholders.

| Category | Enum value | Settings knob (existing name) | Default R,G,B (0-1) |
|---|---|---|---|
| Read | `eNodeCategoryRead` | `readerColor` | 0.70, 0.70, 0.70 |
| Write | `eNodeCategoryWrite` | `writerColor` | 0.75, 0.75, 0.00 |
| Generator | `eNodeCategoryGenerator` | `generatorColor` | 0.30, 0.50, 0.20 |
| Color | `eNodeCategoryColor` | `colorNodesColor` | 0.48, 0.66, 1.00 |
| Filter | `eNodeCategoryFilter` | `filterNodesColor` | 0.80, 0.50, 0.30 |
| Channel | `eNodeCategoryChannel` | `channelNodesColor` | 0.60, 0.24, 0.39 |
| Keyer | `eNodeCategoryKeyer` | `keyerNodesColor` | 0.00, 1.00, 0.00 |
| Merge | `eNodeCategoryMerge` | `defaultMergeColor` | 0.30, 0.37, 0.776 |
| Draw/Paint | `eNodeCategoryDraw` | `drawNodesColor` | 0.75, 0.75, 0.75 |
| Time | `eNodeCategoryTime` | `timeNodesColor` | 0.70, 0.65, 0.35 |
| Transform | `eNodeCategoryTransform` | `transformNodesColor` | 0.70, 0.30, 0.10 |
| Views | `eNodeCategoryViews` | `defaultViewsColor` | 0.50, 0.90, 0.70 |
| Deep | `eNodeCategoryDeep` | `defaultDeepColor` | 0.00, 0.00, 0.38 |
| Native 3D | `eNodeCategoryNative3D` | `native3DNodesColor` (new) | 0.50, 0.20, 0.60 |
| USD 3D | `eNodeCategoryUsd3D` | `usd3DNodesColor` (new) | 0.20, 0.55, 0.55 |
| Other | `eNodeCategoryOther` | reuses `defaultNodeColor` | 0.70, 0.70, 0.70 |

Decided (was pending at first draft; the user has since resolved it): **two** 3D
categories, not one — Native 3D for Natron's own future scene/geometry nodes, USD 3D
for nodes whose data representation is specifically a USD stage (import/export,
USD-backed scene composition). Violet (0.50, 0.20, 0.60) and teal (0.20, 0.55,
0.55) were picked to sit furthest from the two nearest existing hues (Merge's
blue-indigo and Channel's maroon) while remaining distinguishable from each other —
Native 3D is blue-dominant, USD 3D is green-dominant. Each gets its own
`KnobColor` in `Settings::initializeKnobsNodeGraphColors`
(`Engine/Settings.cpp:922-1012`), named `native3DNodesColor` / `usd3DNodesColor`,
added next to `defaultDeepColor`, with the defaults above set in `setDefaultValues`
(`Engine/Settings.cpp:1729-1777`). `Settings::getNodeCategoryColor(NodeCategoryEnum,
float*, float*, float*)` dispatches to all sixteen.

`Backdrop` is **not** a category: it keeps its own `getDefaultBackdropColor()`
(0.45, 0.45, 0.45), checked by `Node::getNodeCategory()` before the ladder runs (a
caller-visible sentinel, or callers check `dynamic_cast<Backdrop*>` first exactly as
`getColorFromGrouping` does today at `Gui/NodeGui.cpp:397,405-406`).

Other/Read/Draw sharing 0.70-0.75 grey today is pre-existing (`Engine/Settings.cpp`
defaults) and out of scope for this milestone — P1.T1 is told to keep existing
defaults unless there is a strong reason to change them, and "they happen to look
similar" is not one.

### 2a. Every `PLUGIN_GROUP_*` string, mapped

(`grep PLUGIN_GROUP_ Global/Macros.h Engine/`, 2026-10-10 checkout.)

| `PLUGIN_GROUP_*` | String | Category |
|---|---|---|
| `PLUGIN_GROUP_IMAGE` | "Image" | Not a category by itself — only ever reached by nodes where `isReader()`/`isWriter()`/`isGenerator()` already wins the ladder first (ReadNode, WriteNode, Constant, CheckerBoard, ViewerInstance all set this grouping but are caught earlier) |
| `PLUGIN_GROUP_IMAGE_READERS` | "Readers" | Read (label only, used for the Settings knob's display name, never matched against `getPluginGrouping()`) |
| `PLUGIN_GROUP_IMAGE_WRITERS` | "Writers" | Write (label only, same as above) |
| `PLUGIN_GROUP_PAINT` | "Draw" | Draw/Paint |
| `PLUGIN_GROUP_TIME` | "Time" | Time |
| `PLUGIN_GROUP_CHANNEL` | "Channel" | Channel |
| `PLUGIN_GROUP_COLOR` | "Color" | Color |
| `PLUGIN_GROUP_FILTER` | "Filter" | Filter |
| `PLUGIN_GROUP_KEYER` | "Keyer" | Keyer |
| `PLUGIN_GROUP_MERGE` | "Merge" | Merge |
| `PLUGIN_GROUP_TRANSFORM` | "Transform" | Transform |
| `PLUGIN_GROUP_3D` | "3D" | Native 3D (kept as-is; see below) |
| `PLUGIN_GROUP_DEEP` | "Deep" | Deep |
| `PLUGIN_GROUP_MULTIVIEW` | "Views" | Views |
| `PLUGIN_GROUP_TOOLSETS` | "ToolSets" | Other (a meta-grouping for the plugin picker's "toolsets" menu, not a node kind) |
| `PLUGIN_GROUP_OTHER` | "Other" | Other |
| `PLUGIN_GROUP_DEFAULT` | "Misc" | Other (OfxHost's fallback when a plugin declares no grouping at all, `Engine/OfxHost.cpp:1002`) |
| `PLUGIN_GROUP_OFX` | "OFX" | Other (not matched anywhere against a node's major grouping today) |

No node sets `PLUGIN_GROUP_3D` today (grep hits only `Global/Macros.h`'s definition
and a mention in `Engine/Nodes/README.md`) — Natron has no scene/geometry nodes
yet, which is exactly why this was open. Since the macro string is reused for one
of the two new categories, P2.T2 needs one more string to disambiguate the second:
add `PLUGIN_GROUP_3D_USD "3D/USD"` to `Global/Macros.h` next to `PLUGIN_GROUP_3D`,
mirroring the existing `PLUGIN_GROUP_MERGE "/Merges"` sub-grouping convention
(`Engine/Nodes/Merge/Merge.cpp:602`). A future USD node sets `grouping =
PLUGIN_GROUP_3D_USD`; a future native scene node sets `grouping = PLUGIN_GROUP_3D`.
Both still file under the same "3D" top-level menu in the plugin picker (string
prefix match), while the major-group → category table treats them as two distinct
exact-match entries.

## 3. User-border pen width and zoom scaling

The node graph has no cosmetic pen anywhere today — `Gui/Edge.cpp:134,173,830` all
construct a plain `QPen` whose width is a scene-space number, and
`NATRON_STATE_INDICATOR_OFFSET` (`Gui/NodeGui.cpp:109`, = 5) is likewise a
scene-space offset run through `TO_DPIX`/`TO_DPIY` (`Engine/AppManager.h:62-63`,
screen-DPI correction only — it has nothing to do with the `NodeGraph` view's zoom
transform). Every stroke and offset in the graph therefore grows and shrinks
together under `QGraphicsView` zoom, and the user border must follow the same
convention or it will visibly drift relative to everything else as the user zooms.

**Pin: the user-border pen is 4 scene units wide** (`QPen(userColor, TO_DPIX(4))`,
not cosmetic), i.e. 4 screen px when the `NodeGraph` view transform is at its
untransformed 1:1 scale ("100% zoom"). At 50% zoom it renders at ~2px, at 200% at
~8px, exactly like `EDGE_PEN_WIDTH` (2) and the state-indicator offset (5) already
do. 4 was chosen over 2 (same as an edge) so the border reads as a distinct node
affordance rather than "an edge glued to the node", and under 5 (the halo offset)
so it is strictly thinner than the band the halo occupies (§5).

## 4. Inset vs outset

**Pin: inset.** The border is stroked so its *outer* edge coincides exactly with
`_boundingBox`'s rect — the same rect `NodeGraphRectItem::paint()`
(`Gui/NodeGraphRectItem.cpp:39-44`) already fills for the body — and its *inner*
edge is 4px further in. Qt centers a `QPen` stroke on its path by default, so this
means stroking a rect inset by half the pen width (`rect().adjusted(w/2, w/2,
-w/2, -w/2)`) rather than `_boundingBox->rect()` directly. The practical effect:
zero pixels of the border ever fall outside the node's footprint. This is the
opposite of `_stateIndicator`, which is entirely outside the footprint (§5) — an
inset border and an outset halo cannot overlap by construction, which is the whole
point (an outset border was rejected for exactly this reason: it would sit in the
same band the halo already owns).

## 5. Staying distinguishable from the selection halo

**What `_stateIndicator` looks like today.** It is a second `NodeGraphRectItem`
(`Gui/NodeGui.cpp:697-699`), at `zValue = depth - 1` (strictly behind the node
body), hidden by default, sized to the node's rect inflated by
`NATRON_STATE_INDICATOR_OFFSET` (5 DPI px) on all four sides
(`Gui/NodeGui.cpp:1064-1067`). It is **always filled, never stroked** — nothing in
`refreshStateIndicator()` (`Gui/NodeGui.cpp:2347-2394`) calls `setPen`, only
`setBrush`, with white for "selected", yellow for "rendering", green for the
merge-connect hint, dark red / olive for error / warning persistent messages.
Visually it is a flat-coloured ring exactly 5px wide, framing the node, visible
only in that ring because the opaque node body covers the rest of the inflated
rect.

**Rule.** Three independent properties keep the two unmistakable, so no single one
has to carry the whole burden: (1) position — the halo is strictly outside the
footprint, the border is strictly inside it (§4), so they occupy disjoint regions
with the footprint edge itself as the dividing line; (2) rendering primitive — the
halo is a filled shape with no stroke, the border is a stroke with no fill of its
own; (3) colour vocabulary — the halo's colours are fixed status colours chosen by
Natron (white/yellow/green/dark-red/olive), never the arbitrary colour a user
picks from a colour dialog, so even in a screenshot with no selection state visible
the border's colour alone marks it as "something the user chose" rather than "a
status light".

## 6. Minimum contrast between border and body

**Formula.** Reuse the same WCAG relative-luminance function as §7 (so the
engine/GUI code needs only one implementation of it). For an sRGB triple `(R,G,B)`
in 0-1: linearize each channel with `lin(c) = c/12.92` if `c <= 0.04045` else
`((c+0.055)/1.055)^2.4`, then `L = 0.2126*lin(R) + 0.7152*lin(G) + 0.0722*lin(B)`.

**Pin:** compute `L_body` and `L_user`. If **both**
(a) the WCAG contrast ratio `(max(L)+0.05)/(min(L)+0.05) < 1.3`, **and**
(b) the Euclidean sRGB distance `sqrt(ΔR² + ΔG² + ΔB²) < 0.10`
are true — i.e. the user colour is close to the body colour in *both* luminance
and hue, the two-condition test exists because a hue-only difference at matched
luminance (e.g. a saturated red border on an equally-bright green body) is already
visually separable and must not be "corrected" into grey — then the border is not
drawn in the raw user colour. Instead it is drawn after mixing 35% toward white (if
`L_body < 0.5`) or 35% toward black (if `L_body >= 0.5`), i.e. `mixed = user*0.65 +
target*0.35` per channel, where `target` is (1,1,1) or (0,0,0). This guarantees a
visible ring even when a user picks (deliberately or by accident) almost exactly
the node's own category colour, without changing the border's hue identity in the
common case where hue alone already separates it.

## 7. Label text colour

**Formula and threshold.** Same WCAG relative luminance as §6. **Pin: `L >
0.179` → dark text, else → white text.** 0.179 is the luminance at which white
text (L=1) and black text (L=0) give the *same* WCAG contrast ratio against the
background (solving `1.05/(L+0.05) = (L+0.05)/0.05` gives `L = sqrt(0.0525) - 0.05
≈ 0.1791`), so it is the standard crossover point, not an arbitrary pick. Pin the
two text colours to exactly what `_nameItem` already uses for the dark case — pure
black `QColor(0,0,0,255)` (`Gui/NodeGui.cpp:682`) — and pure white `QColor(255,
255,255,255)` for the light case, so nodes that were already fine keep their exact
current pixels.

**Verification against the two colours this milestone cares about most:**
- Deep default (0, 0, 0.38): `lin(0.38) ≈ 0.1193`, `L ≈ 0.0722*0.1193 ≈ 0.0086 <
  0.179` → **white text.** This is the documented bug fix (black-on-navy today).
- Default node/Read/Draw grey (0.70, 0.70, 0.70): `lin(0.70) ≈ 0.448`, `L ≈ 0.448 >
  0.179` → dark text, matching the current hard-coded black exactly, so the
  majority of nodes do not change.
- Transform default (0.70, 0.30, 0.10): `L ≈ 0.148 < 0.179` → white text. This is
  an intentional, in-scope change (P3.T6 is explicitly a general contrast fix, not
  a Deep-only patch) and should be called out in that task's screenshot evidence.
- Native 3D default (0.50, 0.20, 0.60): `L ≈ 0.092 < 0.179` → white text.
- USD 3D default (0.20, 0.55, 0.55): `L ≈ 0.215 > 0.179` → dark text. (Chosen
  deliberately so the two new categories exercise both branches of the rule.)

A user `<font color>` in the label HTML still wins outright (`KnobGuiString::
parseFont`, checked before this rule applies), exactly as `setNameItemHtml()`
(`Gui/NodeGui.cpp:3107-3222`) already special-cases explicit font tags.

## 8. Ladder precedence: domain beats I/O role

**Today's inconsistency.** `DeepWrite` sets `desc.isWriter = true`
(`Engine/Nodes/Deep/DeepWrite.cpp:91`), so it hits the `isWriter()` rung before the
grouping chain ever runs and is coloured as a generic Writer. `DeepRead` sets no
such flag, falls through every I/O check, and is coloured as Deep. Two nodes in the
same family disagree today for no principled reason — simply because only one of
them happened to set the OFX-era `isWriter` flag.

**Pin: domain categories beat the Reader/Writer/Generator rungs.** `Node::
getNodeCategory()`'s ladder, in order: (1) Backdrop sentinel; (2) domain groups
that carry their own colour identity — today Deep, Native 3D, USD 3D — resolved
directly from `PLUGIN_GROUP_*` before anything else; (3) `isReader()` /
`isWriter()` / `isGenerator()`; (4) the remaining `PLUGIN_GROUP_*` → category
table; (5) the label/ID keyword heuristic; (6) `eNodeCategoryOther`. Concretely
this makes `DeepWrite` Deep-coloured (changing today's behaviour) and keeps
`DeepRead` Deep-coloured (no change), so the two agree.

**Reason.** Domain colour is the rarer, more specific signal — a handful of Deep
nodes among dozens of generic readers/writers — and it is exactly the kind of "what
flavour of data flows through here" information this milestone exists to protect,
echoing the now-removed Okabe-Ito deep-edge colour (`Gui/Edge.cpp`) that users relied
on to spot deep streams. The alternative (I/O role wins) would need `DeepRead` to
also start setting `isReader()` to reach consistency, which *loses* the Deep
signal on both nodes instead of gaining it on one, and does not generalise: a
future USD-stage writer should read as USD 3D, not disappear into the generic
Writer grey/yellow bucket where nothing distinguishes it from a plain image write.
This supersedes the ladder order sketched in M24.P2.T2's own approach text (rungs
2 and 3 there are reader/writer/generator before grouping); P2.T2 implements the
order fixed here, as the milestone's task text anticipates ("in the order fixed by
P1.T1").

## 9. Decided, not pending (resolved after the first draft of this note)

Both items the milestone originally flagged as open user decisions are now
decided; folded into §2 and the note below rather than left as options.

- **3D categories: two, not one, not zero.** §2 and §2a give both their enum
  values, knob names, defaults and grouping-string disambiguation.
- **Edge pen-width ladder: removed.** `kindWidthMultiplier()`
  (`Gui/Edge.cpp:785-797`) and its use at `:830` are deleted per M24.P4.T2; every
  edge uses `EDGE_PEN_WIDTH` regardless of `DataKindEnum`. Edge *colour* (the
  Okabe-Ito tints, `Gui/Edge.cpp:800-813,880`) becomes the sole data-kind signal
  left anywhere in the graph — node-level kind is colour (this milestone) and edge
  kind is colour alone, no longer colour-plus-width. The dash pattern for
  mask/hidden inputs is unrelated state and is untouched.
