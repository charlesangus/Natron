# M523 - USDSetAttribute Type From Stage

When USDSetAttribute names an attribute that already exists on the matched prims, pick its type for the user. Today the user chooses `valueType` from `kValueTypes[]` by hand (`Engine/Nodes/Scene/USDSetAttribute.cpp`, `initializeKnobs()` ~line 140), and changing it rebuilds the Value knob through `rebuildValueKnob()` → `SchemaKnobMapper::createKnob`. The node reads the stage only at render time (`SceneQueryPool::instance().borrow(upstream, …)` in `renderScene`) and checks only the schema's definition (`GetPrimDefinition().GetAttributeDefinition`), not attributes authored on the stage. The type is filled in but stays editable; a type that differs from the existing attribute shows a warning (user, 2026-11-22).

## Scope

**In:**
- Looking up the attribute on the upstream stage at the current time: authored attributes and schema-defined ones, including `primvars:<name>` when Primvar is on.
- When found with a type in `kValueTypes[]`: set `valueType` (rebuilding Value). The user can change it afterwards.
- A persistent node warning while the chosen type differs from the existing attribute's: "<attr> on <prim> is a <type> attribute; authoring <chosen> over it".
- Running the lookup off the UI thread; the result applies when it arrives.

**Out:**
- Seeding the Value knob from the attribute's current value: not chosen (user, 2026-11-22).
- Attribute-name completion or a picker: none.
- Array, matrix and quaternion attribute support in the Value knob (`SchemaKnobMapper::mapType` rejects them): none.

**Edge cases:**
- The lookup runs when `attributeName`, `primPattern` or `primvar` change by user edit, or the input is connected; never on project load, so a saved type is never overwritten.
- No match, or the attribute doesn't exist: nothing changes, no warning.
- Several matched prims with different types for the attribute: use the first match in stage order, warn naming the conflict.
- Existing attribute of an unsupported type (array, matrix): leave `valueType`, warn "<type> is not supported".
- Prims inside unloaded payloads are not seen (no payload loading without user action); the lookup sees only the loaded stage.
- The existing schema-mismatch render error stays: a schema-defined attribute of another type still fails at render.
- Upstream edits after the lookup: the warning re-evaluates at render; the type is not re-picked automatically.

## Phase 523.1: Lookup

- [ ] M523.P1.T1 — Find an existing attribute's type on a stage
  - files: `Engine/Nodes/Scene/USDSetAttribute.{h,cpp}`, `Tests/Scene/USDSetAttributeLookup_Test.cpp` (or beside the existing Scene tests), `Tests/CMakeLists.txt`
  - approach: a static `lookupExistingAttribute(const UsdStageRefPtr&, pattern, name, primvar, time)` returning a struct `{found, typeName, primPath, conflict, unsupported}`: matches prims the way `renderScene` does, then checks `prim.GetAttribute(token)` (authored or schema-defined, via `UsdAttribute::GetTypeName()`), and maps the `SdfValueTypeName` to a `kValueTypes[]` index or marks it unsupported.
  - verify: gtest on an in-memory stage — an authored `color3f` custom attribute, a schema `radius` on a Sphere, a `primvars:foo` float, a `point3f[]` (unsupported), two prims with conflicting types (first wins, conflict set), and a missing name (not found).
  - size: M

- [ ] M523.P1.T2 — Pick the type on edit
  - files: `Engine/Nodes/Scene/USDSetAttribute.{h,cpp}`, `Tests/gui/usd_setattribute_type.py`
  - approach: in `knobChanged` for user edits of `attributeName`, `primPattern`, `primvar` and in `onInputChanged`, start the lookup on a worker: borrow the upstream stage via `SceneQueryPool` off the main thread at the current time, run `lookupExistingAttribute`, post the result back to the main thread; ignore stale results with a generation counter. A found, supported type sets `valueType` (which rebuilds Value as today). Skip during project load (`onKnobsAboutToBeLoaded` / loading flag).
  - verify: an Xvfb script builds Sphere → USDSetAttribute, types `radius`, asserts `valueType` becomes `double` within a timeout; types an authored `displayColor` primvar with Primvar on and asserts `color3f`; saves with a hand-changed type, reloads, asserts it is kept.
  - size: M

- [ ] M523.P1.T3 — Warn on a type mismatch
  - files: `Engine/Nodes/Scene/USDSetAttribute.cpp`, `Tests/gui/usd_setattribute_type.py`
  - approach: in `renderScene`, after matching, compare the chosen type with each matched prim's existing authored attribute (schema mismatches keep their error) and set a persistent warning (the node's warning channel, as other Scene nodes do) with the In text, plus the conflict and unsupported warnings from Edge cases; clear it when they agree.
  - verify: the Xvfb script changes `radius`'s type to `float` and asserts the warning text on the node; sets it back and asserts it clears; a screenshot of the warning goes to the user.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDSetAttributeLookup_Test` passes; `Tests/gui/usd_setattribute_type.py` passes under Xvfb at 1920x1080; the user OKs the warning screenshot.

## Decisions

- 2026-11-22 — **Type auto-set but editable (user):** an existing attribute's type is filled in, the user may override it, and a mismatch shows a warning; the value is not seeded.
- 2026-11-23 — Main (F4) merged in: the off-thread type lookup now borrows with the showing viewer's context and load set, and skips the lookup when no viewer shows the node (PM; the viewer never loads payloads without user action).
