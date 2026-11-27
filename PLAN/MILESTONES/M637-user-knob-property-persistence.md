# M637 - User Knob Property Persistence

Some user-knob properties are not saved, and are lost on save/load, copy/paste and Ctrl-drop copy (from the F16 - User Knob Editor review). The fields are: increments and decimals, Double matrix display, primPathHint, customHTMLText (`Engine/KnobTypes.h:1162`), and per-dimension min/max for Int/Double/Color. The scout found that `KnobUserPropertiesSerialization` (`Serialization/KnobSerialization.h:305`) has no field for increment, decimals, primPathHint or customHTML. Range is one min/max pair per knob (`KnobSerialization.h:175-183`, written at `Engine/KnobSerializationConvert.cpp:520-540`). Parametric min/max is saved (:564-567, applied :1141), so its loss on copy is in the copy path (`Engine/UserKnobEdit.cpp:1632` `copyUserKnobs`).

## Scope

**In:**
- Every property above, through save/load, copy/paste and Ctrl-drop copy.

**Out:**
- Built-in (non-user) knobs.

**Edge cases:**
- **Properties at their defaults:** not written, matching `UserKnobPropertiesAreWrittenOnlyWhenTheyDifferFromTheDefaults`.
- **Per-dimension ranges all equal:** written as the single pair, so common files stay small.
- **Old projects:** no backward compatibility needed.

## Phase 637.1: Serialize

- [ ] M637.P1.T1 — Add the missing properties to user-knob serialization, both directions
  - files: `Serialization/KnobSerialization.{h,cpp}` (`KnobUserPropertiesSerialization`, range fields), `Engine/KnobSerializationConvert.cpp` (:520-567, :1141), the YAML encode/decode for those structs; `Tests/Serialization/KnobYaml_Test.cpp` (:601), `Tests/Serialization/ProjectBundleRoundTrip_Test.cpp` (:140)
  - approach: add increments, decimals, matrix display, primPathHint and customHTMLText to the struct, and per-dimension min/max lists alongside the existing pair (written only when dimensions differ). Convert both ways. Extend the round-trip tests with a knob carrying each property at a non-default value.
  - verify: round-trip tests show every property restored after save/load; `ctest -R "KnobYaml|ProjectBundle|NodeConvert"` green
  - size: M
- [ ] M637.P1.T2 — Carry the properties through copy/paste and Ctrl-drop copy
  - files: `Engine/UserKnobEdit.cpp` (`copyUserKnobs` :1632 and the paste/drop paths), `Tests/UserKnobEdit_Test.cpp` (:366)
  - approach: route copy through the same serialization as T1, or add the missing fields where it copies by hand, including parametric min/max. Add tests for copy/paste and the drop-copy path with every property set.
  - verify: the new copy tests pass; `ctest -R UserKnobEdit` green
  - size: M

**Verification gate:** full build and ctest green; GUI check: make a user knob with every property set, save, reload, copy it to another node, and confirm in the editor (Xvfb screenshot sent to the user).
