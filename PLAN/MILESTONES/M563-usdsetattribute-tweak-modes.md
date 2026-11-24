# M563 - USDSetAttribute Tweak Modes

USDSetAttribute always replaces: it creates the attribute on each matched prim and sets the Value knob over whatever was there (`USDSetAttribute::renderScene`, `Engine/Nodes/Scene/USDSetAttribute.cpp`). This milestone adds a **Mode** so the node can also change what is there: Add a value, Multiply by one, or Remove (block) the attribute, with an **Ignore Missing** switch for tweaks aimed at attributes a prim does not have. It models Gaffer's AttributeTweaks and ShaderTweaks (`mode`, `ignoreMissing`; `.local/docs/gaffer/1.7.5.0/Reference/NodeReference/GafferScene/AttributeTweaks.html`) and Solaris Edit Properties' Add, Multiply, Block and Set-if-Exists controls (`.local/docs/houdini/nodes/lop/editproperties.md`). M523 - USDSetAttribute Type From Stage just landed type-from-stage on the node: the type of an existing attribute fills `valueType`, which is what makes Add and Multiply usable without the user guessing the type.

The new modes read the attribute's composed value on the input stage (borrowed through `SceneQueryPool`, as `renderScene` already does to check the schema) and author the combined result in the node's own layer. The node stays a raw single-attribute editor; metadata and schema-driven editing belong to M564 - USDConfigurePrim Node and USDEditPrim.

## Scope

**In:**
- A `mode` choice, label "Mode": Replace (the node's current behaviour, the default), Add, Multiply, Remove. `ignoreMissing`, label "Ignore Missing" (bool, default off), shown for every mode.
- Replace: creates a missing attribute or sets an existing one (the current behaviour, Solaris's Set or Create); it never errors for a missing attribute. With Ignore Missing on, Replace turns that off: a matched prim that lacks the attribute is skipped and nothing is created on it (Solaris's Set if Exists).
- Add and Multiply: for the numeric types the Value knob has (int, uint, half, float, double, and the 2/3/4-vectors and colours of float and double), component-wise on the existing composed value: `existing + value`, `existing * value`. Authored at the current time when neither side varies in time; at every sample frame when the Value knob is animated or the existing attribute's value varies in time (`UsdAttribute::ValueMightBeTimeVaried`).
- Remove: blocks the attribute (`UsdAttribute::Block()`), so it composes as unauthored and takes its schema fallback. This is Solaris's Block; an edit layer cannot delete a lower layer's opinion, and a block is what USD offers.
- Ignore Missing: with it off, Add, Multiply and Remove on a matched prim whose attribute does not exist (after composing the input) fail the node with an error naming the prim and attribute, and Replace creates it; with it on, such a prim is skipped silently in every mode. A prim where the attribute exists is always tweaked.
- Add and Multiply on a primvar (Primvar on): applied to every element of the existing `primvars:<name>` array, and authored back as an array of the same length; Replace stays the one-element array it is today.
- Type handling: Add/Multiply authors the type the existing attribute has; if the chosen `valueType` differs from it, the node errors (the M523 mismatch warning already shows it; with a value-combining mode it becomes an error because the operands cannot combine).
- Types with no arithmetic (bool, string, token, asset path): Add and Multiply are disabled in the Mode choice's tooltip and the node errors if chosen; Remove and Replace work.
- Knob visibility: Value is hidden for Remove.
- Node doc page `USDSetAttribute.md` (none exists on main) describing every mode, ctest coverage, one Xvfb GUI script.

**Out:**
- List Append (and prepend/remove of list entries): the Value knob has no array support (`SchemaKnobMapper::mapType` rejects arrays), and USD attributes are not list-ops. Scoped out per the brief's own condition; no milestone, revisit if the Value knob ever gains arrays.
- Subtract, Min, Max, Create-if-missing modes of the reference nodes: not asked for; Add of a negative value subtracts. No milestone.
- Set-if-no-authored-value and Set-if-exists variants of Solaris: Ignore Missing covers Set-if-exists for the tweak modes only; the others have no milestone.
- Relationship edits: M562 - USDEditPrim Relationship Knobs.
- Array-valued tweaks on non-primvar attributes (`points`, `extent`): the Value knob has no array types.
- Localising an attribute inherited from an ancestor (Gaffer `localise`): USD attributes do not inherit, so it does not apply.
- Seeding the Value knob from the current value: not chosen at M523 (user, 2026-11-22).

**Edge cases:**
- Several matched prims: each prim's own existing value is combined, so Add 1.0 to `radius` over two spheres of radius 1 and 3 authors 2 and 4. Per-prim failures are all reported, not just the first.
- The existing attribute is blocked, or has no composed value, or only an `uniform` fallback: treated as missing for Add and Multiply (error or skip per Ignore Missing).
- An attribute authored in a payload that is not loaded: not seen (no payload loading without user action); the prim is treated as missing, and with Ignore Missing off the error says the attribute may be inside an unloaded payload when the matched prim has one.
- A schema-defined attribute (such as `radius`) is "existing" even with no authored opinion: Add and Multiply use its fallback value, as `UsdAttribute::Get` returns it.
- Existing value time-sampled, Value knob constant: authors one sample per existing sample time within the frame range (sample frames come from `SceneNodeAuthoring::getSampleFrames` of the edit), each `existing(t) op value`.
- Add/Multiply on `half`: computed in float and narrowed; on `uint`, a negative result clamps at zero with a node warning.
- Multiply a `color3f` by a `color3f`: component-wise.
- Changing Mode never changes the saved Value or type (no seeding); switching to Remove only hides Value.
- Remove with Primvar on blocks `primvars:<name>` (and leaves `primvars:<name>:indices` alone, a block of the array being enough).
- Layer cache key: the layer reads the input's values, so its memo key covers the full upstream stack (not only metadata facts), like USDEditTransform's, and the frame range when the existing value or the Value knob varies in time.
- A project saved before this milestone has no `mode`: nothing is imported or converted; the knob takes its default Replace (no backward compatibility).

**Assumptions:**
- The tweak result for a time-varying existing value is authored as time samples in the node's own layer; a downstream or stronger layer overriding it still wins as usual.
- `SceneLayerMemo` can key a layer on the full upstream stack with a frame range; T3 confirms against `USDEditTransform` and `USDSetAttribute`'s current `reads` before building on it.

## Phase 563.1: Modes

- [ ] M563.P1.T1 — Combine functions for Add, Multiply
  - files: `Engine/Nodes/Scene/USDSetAttributeTweak.{h,cpp}` (new), `Tests/Scene/USDSetAttributeTweak_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: pure functions free of knobs: `bool combine(Op op, const VtValue& existing, const VtValue& value, VtValue* result, std::string* error)` dispatching on the held type over the types in `USDSetAttributeLookup::getValueTypes()` that support arithmetic (int, uint, half, float, double, GfVec2/3/4 f and d, colours as GfVec3f/4f/d), plus `bool combineArray(Op, const VtValue& existingArray, const VtValue& value, VtValue* result, error)` that broadcasts the scalar over a `VtArray<T>` of the same element type. Unsupported or mismatched types set `error` ("<type> cannot be added to"). uint underflow clamps at 0 and reports a flag.
  - verify: gtest `INSTANTIATE_TEST_CASE_P` over every arithmetic type: Add and Multiply results, an array broadcast, a half narrowed from float, a uint clamp with the flag set, a string/bool/token reports unsupported, mismatched held types report an error.
  - size: M

- [ ] M563.P1.T2 — Mode and Ignore Missing knobs, authoring
  - files: `Engine/Nodes/Scene/USDSetAttribute.{h,cpp}`, `Tests/Scene/SetAttribute3D_Test.cpp`
  - approach: add `mode` and `ignoreMissing` knobs (`kUSDSetAttributeParamMode`, `...IgnoreMissing`; labels "Mode", "Ignore Missing") in `initializeKnobs`, serialized, with tooltips; `knobChanged` shows or hides Value and Ignore Missing per mode. In `renderScene`, per matched prim: Replace keeps today's path; Add/Multiply read the composed value at the sample time through the borrowed stage's `UsdAttribute::Get`, call `combine`/`combineArray`, and author through `SceneNodeAuthoring::authorValue` (per-frame when the Value knob is animated or `ValueMightBeTimeVaried`); Remove calls `Block()` on the existing attribute. A missing attribute is an error naming prim and attribute for Add, Multiply and Remove unless Ignore Missing is on; Replace creates it unless Ignore Missing is on, when the prim is skipped. Collect failures across prims and report them together. Include `mode`, `ignoreMissing` and the matched prims' existing-value digests in the layer facts so the cache key tracks them (see Edge cases).
  - verify: gtest on in-memory stages: Add and Multiply on `radius` of two spheres with different radii; Add on a missing custom attribute errors, and passes with Ignore Missing on while a sibling prim that has it is still tweaked; Remove blocks `radius` (composed `Get` falls back to the schema default) and on a missing attribute follows Ignore Missing; a type mismatch errors; a time-sampled existing attribute gives matching samples; Replace on a missing attribute creates it, and with Ignore Missing on skips that prim and creates nothing, while a sibling prim that has it is still set; Replace with Ignore Missing off behaves exactly as before (existing `SetAttribute3D_Test` cases pass unchanged).
  - size: L

- [ ] M563.P1.T3 — Primvar tweaks
  - files: `Engine/Nodes/Scene/USDSetAttribute.cpp`, `Tests/Scene/SetAttribute3D_Test.cpp`
  - approach: with Primvar on, Add/Multiply read `primvars:<name>` as an array (the existing authored type's array form), call `combineArray`, and author the array back with the same interpolation and length; Remove blocks the primvar attribute. Replace is unchanged (one-element array).
  - verify: gtest on a prim with a 4-element `float[]` primvar: Add 0.5 gives each element +0.5 and the interpolation metadata is preserved; Multiply on `displayColor` (`color3f[]`) scales each element; Remove blocks it; a missing primvar follows Ignore Missing.
  - size: M

- [ ] M563.P1.T4 — Interaction with the M523 type lookup
  - files: `Engine/Nodes/Scene/USDSetAttribute.cpp`, `Engine/Nodes/Scene/USDSetAttributeLookup.{h,cpp}`, `Tests/Scene/USDSetAttributeLookup_Test.cpp`
  - approach: keep the type lookup as is, and make the mismatch handling mode-aware: for Replace the existing warning stays; for Add/Multiply a mismatch (or an unsupported or non-numeric existing type) is a render error, while the lookup still fills `valueType` on user edit so the user rarely sees it. Remove ignores `valueType` for authoring (it only needs the attribute name) and skips the type check.
  - verify: gtest: Replace with a mismatched type still warns and authors; Add with a mismatched type errors with both type names; Remove with any `valueType` blocks the attribute; the existing `USDSetAttributeLookup_Test` cases pass unchanged.
  - size: M

## Phase 563.2: Docs and GUI check

- [ ] M563.P2.T1 — Node doc page
  - files: `Engine/Nodes/Scene/docs/USDSetAttribute.md`
  - approach: document Prim Pattern, Attribute Name, Value Type, Primvar, Value, Mode and Ignore Missing: each mode's rule, the numeric types Add and Multiply take, how Remove blocks, what Ignore Missing skips, the type-from-stage fill and mismatch warning from M523, and why there is no list mode. Style of the other Scene docs; no mention of other applications.
  - verify: a reviewer checks the page's mode names, script names and defaults against the knobs.
  - size: S

- [ ] M563.P2.T2 — Xvfb GUI script
  - files: `Tests/gui/usd_setattribute_modes.py`
  - approach: under Xvfb at 1920x1080 build Sphere -> USDSetAttribute from Python; set `radius`, Mode Add with value 2, assert the composed `radius` through the viewer-side stage query; switch to Multiply, then Remove (Value hidden, attribute back at its fallback); point it at a missing attribute with Ignore Missing off (node error text) and on (no error); save and reload the project and assert Mode and Ignore Missing persist. Screenshot of the node's knob page in Add mode.
  - verify: the script passes in the remote `--gui` gate; the user OKs the screenshot (one file).
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDSetAttributeTweak_Test`, `SetAttribute3D_Test` and `USDSetAttributeLookup_Test` pass; `Tests/gui/usd_setattribute_modes.py` passes under Xvfb at 1920x1080; the user OKs the Mode knob screenshot.

## Decisions

- 2026-11-24 — **USDSetAttribute stays the raw single-attribute node and gains tweak modes (user):** part of the settled "Shape A plus Configure Layer". Modes Replace, Add, Multiply, Remove, List Append, plus Ignore Missing were requested; List Append is arrays-only and the Value knob has no arrays, so it is scoped out here as the request allowed (user's own condition).
- 2026-11-24 — Remove is a USD block, as Solaris's Block does; an edit layer cannot erase a lower layer's opinion (PM draft, "match Solaris").
- 2026-11-24 — M523 landed first: the type lookup is kept and made mode-aware rather than reworked (PM draft).
- 2026-11-24 — **Replace creates a missing attribute; Ignore Missing turns that off (user):** Replace keeps today's create-or-set behaviour, and Ignore Missing applies to every mode, making Replace skip prims that lack the attribute.
