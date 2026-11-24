# M567 - USD Light Linking

Author UsdLux light and shadow links from prim patterns, make Cycles honour them, and record which bundled delegates show them. Light and shadow linking are the `lightLink` and `shadowLink` collections of `UsdLuxLightAPI`. Phase 1 probes Storm, Cycles, MoonRay and the Utility renderer (F7; M281 - Utility Materials And Lights implemented link collections there) on one scene. Phase 2 patches the Cycles delegate fork to honour links. Phase 3 builds a `USDLightLink` node on the Solaris Light Linker's rule structure, on a plain knob panel: several rule rows and several collection rows, authored through `SceneNodeAuthoring::authorCollection()` and the collection references in prim patterns from M566 - USDCollection Node, with the prim-pattern knobs of M562 - USDEditPrim Relationship Knobs. The drag-and-drop three-column panel is M588's. Node docs state which delegates show links, from the probe table.

What the sources say, to be proven by the probe: hdMoonray reads `lightLink` and `shadowLink` on every light (`build/forks/hdMoonray/lib/hydramoonray/Light.cc` ~lines 399-430, as categories); the Cycles Hydra delegate (`.local/forks/cycles/src/hydra/`) reads none, although Cycles `Object` has `receiver_light_set`, `light_set_membership`, `blocker_shadow_set` and `shadow_set_membership` sockets (`src/scene/object.h`); Storm is unknown.

## Scope

**Solaris Light Linker controls, each In or Out:**
- Custom Qt rule-card interface (tree of lights, tree of prims, drag and drop, Alt-drag shadow rule, read-only rows for existing links, "Filter based on selection", card delete and enable checkbox): Out. M588 owns the three-column drag-and-drop panel. This node is a plain knob panel.
- Number of Collections and per-collection Prim, Name, Includes, Excludes: In, as collection rows (`collectionCount`; `collectionPrim<i>`, `collectionName<i>`, `collectionIncludes<i>`, `collectionExcludes<i>`), authored with `authorCollection()` as M566 does. The Create-a-Collection buttons are Out (UI).
- Number of Links: In, as rule rows (`ruleCount`).
- Rule Enabled: In (`enable<i>`).
- Rule Prim, the source light or a collection of lights: In as `Light` (`light<i>`), a prim pattern; a light collection is named by a collection reference (M566).
- Rule Includes and Rule Excludes: In (`includes<i>`, `excludes<i>`), prim patterns.
- Rule Type, Light Link or Shadow Link: In (`type<i>`).
- Rule Reversed: In as `Prim-Centric` (`reversed<i>`): the row's Light pattern is read as the prims, Includes and Excludes as the light patterns, and the node synthesizes light-centric links from it, since USD has no prim-centric link.
- The note that shadow linking works on Karma CPU only: replaced by the probe table in the docs.

**In:**
- Phase 1 probe: a test scene and a probe test over Storm, Cycles, MoonRay and Utility, recorded as a table in Decisions and in the node docs.
- Phase 2: the Cycles Hydra delegate fork reads `lightLink` and `shadowLink` and maps them onto Cycles light sets; the image pin bump in `tools/ci/local/Dockerfile` (user, 2026-11-24: patch Cycles here, not as a follow-up).
- `USDLightLink` with the row lists above. Rows come from the reusable `KnobRowList` helper that this milestone builds (M567.P3.T1), which M575 - USD Restructure And Graft later reuses for USDGraft.
- A node warning naming the delegates the probe found not to show links.
- Docs `Engine/Nodes/Scene/docs/USDLightLink.md`, toolbar entry in the USD Lights submenu (M520), icon, tests.

**Out:**
- The drag-and-drop panel: M588.
- Storm support: only the result is recorded (user, 2026-11-24); Storm is not patched.
- Light filters, light groups and the Light Mixer: none.
- Material linking: none.
- Authoring links for a light in an unloaded payload: not seen, no payload is loaded.

**Edge cases:**
- `Light` matches no light: the row warns "no lights matched"; matched prims that are not lights are skipped with a warning naming the first.
- Empty `Includes` with a set `Excludes`: the light keeps its default root include and gains the excludes ("everything except").
- Non-empty `Includes`: `includeRoot` is turned off on that collection so only the includes are lit. This replaces the light's existing includes; the node edits, it does not append.
- Prim-centric row: every light matched by `Includes` gets the prims added to its includes, every light matched by `Excludes` gets them added to its excludes; a light matched by neither is untouched. Rows combine: several rows hitting one light merge, in row order, rather than replacing each other (a replace applies once per light and type, from its first row).
- Disabled row: contributes nothing. A collection row with a blank name or prim: contributes nothing, warns.
- A light with a link collection authored upstream: the node's first row for it replaces it.
- Cycles light sets are limited to 64 per scene; past that the extra object categories fall back to the default set with a warning naming the count.
- Delegate that does not honour links: no render error; the node warns, the link is still authored (other tools read it).

**Assumptions:**
- Unconfirmed: Storm in the bundled USD honours `lightLink` through Hydra light categories. The probe settles it; the docs say whatever it finds.
- Unconfirmed: the Cycles delegate ignores links entirely (no reads in the fork's `src/hydra/`); the pinned commit may differ from the working copy, which the probe shows.

## Phase 567.1: Probe

- [ ] M567.P1.T1 — Probe scene and per-delegate render test
  - files: `Tests/Scene/USDLightLinkDelegates_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: two spheres and one light, built in memory, with the light's `lightLink` collection excluding sphere B and its `shadowLink` excluding sphere A. Render each delegate (Storm, Cycles, MoonRay, Utility) with and without the links, and compare mean luminance inside each sphere's pixels (primId or a known rect). Parametrise over delegates with `INSTANTIATE_TEST_CASE_P`; absent delegates return early as the existing delegate tests do (no `GTEST_SKIP`). A delegate that does not honour a link is recorded, not failed: the test asserts the recorded table, which Phase 2 updates for Cycles.
  - verify: the release gate runs the test and prints a table; every available delegate has a light-link and a shadow-link verdict.
  - size: L

- [ ] M567.P1.T2 — Record the probe table
  - files: this milestone file (`## Decisions`), `Engine/Nodes/Scene/RenderDelegates.{h,cpp}`
  - approach: write the table as a Decision; add `honoursLightLinks(delegateId)` and `honoursShadowLinks(delegateId)` to `RenderDelegates` so the node's warning and the docs agree with the tests; the T1 test asserts the constants against the measurement.
  - verify: gtest — the constants equal the probe verdicts; an unknown delegate id returns false for both.
  - size: M

## Phase 567.2: Cycles fork

- [ ] M567.P2.T1 — Map light and shadow links onto Cycles light sets
  - files: `.local/forks/cycles/src/hydra/{light.cpp,geometry.inl,render_delegate.cpp,light.h}`, `tools/ci/local/Dockerfile`
  - approach: Hydra gives each light the token of its `lightLink` and `shadowLink` collection (`GetLightParamValue`) and each geometry the categories it belongs to (`GetCategories`), as hdMoonray reads them. Keep a delegate-wide table of the distinct category combinations of the geometry; give each combination a set index (at most 64, index 0 is "all"); an object's `receiver_light_set`/`blocker_shadow_set` is its combination's index, and a light's `light_set_membership`/`shadow_set_membership` is the bitmask of every combination its category belongs to (a light with no link collection has all bits). A category change on a light or geometry re-syncs the affected objects, as hdMoonray re-syncs geometry. Commit to the fork branch, then bump `NATRON_CYCLES_COMMIT` and its SHA256 in the Dockerfile; rebase on M494's and M568's fork changes, whichever landed first, since all three change the fork and its pin.
  - verify: the fork builds in the image (release gate); the probe test of M567.P1.T1 now shows Cycles honouring light and shadow links, and the recorded table is updated; a Cycles render without links is unchanged against the existing goldens.
  - size: L

## Phase 567.3: The node

- [ ] M567.P3.T1 — A reusable knob row list
  - files: `Engine/Nodes/KnobRowList.{h,cpp}` (new), `Tests/Native/KnobRowList_Test.cpp` (or beside the existing native-node tests), `Tests/CMakeLists.txt`
  - approach: a generic helper modelled on `RFVSwitch`'s row handling (`Engine/Nodes/RFV/RFVSwitch.{h,cpp}`, which stays as it is). A node declares a count knob (script name, label, maximum) and a per-row column spec: a knob factory per column plus a script-name prefix, so `light` gives `light<i>`. The helper creates rows up to the maximum, hides the rows past the count as secret, and gives each row Add After and Remove buttons. Those buttons shift the values, animation and expressions of the rows below and change the count, in one undoable edit, then emit a rows-changed callback. Rows are ordinary knobs, so they animate and take expressions, which `KnobTable` cells can't (see `Engine/Nodes/Metadata/docs/MetadataModify.md`). The helper is not specific to light links: M575 - USD Restructure And Graft's USDGraft reuses it.
  - verify: a gtest on a test holder with two columns (string, bool) checks that the count shows and hides rows, that Add After at row 1 shifts rows 2 onward down with their values and an animated key, that Remove shifts them up, and that a project save/load round trip keeps the rows.
  - size: L

- [ ] M567.P3.T2 — Rule rows and light-centric authoring
  - files: `Engine/Nodes/Scene/USDLightLink.{h,cpp}`, `Engine/Nodes/Scene/USDLightLink.reg.inc`, `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}`
  - approach: node with rule rows on `KnobRowList` (M567.P3.T1): `ruleCount` and per row Enable, Light, Includes, Excludes, Type, Prim-Centric (patterns from M562; labels equal the readable script names). A light-centric row writes through `authorCollection()` on `UsdLuxLightAPI::GetLightLinkCollectionAPI()` or `GetShadowLinkCollectionAPI()` of each matched light, with the include-root rules from Edge cases. Row-count limit 1024. Warnings per Edge cases plus the delegate warning from `RenderDelegates`.
  - verify: gtest on an in-memory stage — links on two lights from two rows, empty includes keeping the root, non-empty includes dropping it, shadow type writes `shadowLink`, a light named by a collection reference (M566), a disabled row, a non-light match warns, removing a row removes its effect.
  - size: L

- [ ] M567.P3.T3 — Prim-centric rows
  - files: `Engine/Nodes/Scene/USDLightLink.{h,cpp}`, `Tests/Scene/USDLightLink_Test.cpp`
  - approach: a row with Prim-Centric on resolves its Light pattern as the target prims and Includes and Excludes as light patterns, then runs the light-centric write per light, merging with what earlier rows wrote to that light (Edge cases).
  - verify: gtest — three lights, a prim included by one and excluded by another, a light matched by neither unchanged, two rows hitting one light merge, chained nodes merge.
  - size: M

- [ ] M567.P3.T4 — Collection rows
  - files: `Engine/Nodes/Scene/USDLightLink.{h,cpp}`
  - approach: `collectionCount` rows of Prim, Name, Includes, Excludes authored with `authorCollection()` (replace semantics, namespaced names allowed) before the rules run, so a rule can name a collection from a row above in the same node by reference.
  - verify: gtest — a collection row then a rule using it by `%name`; a blank name warns and authors nothing.
  - size: M

- [ ] M567.P3.T5 — Docs, toolbar entry, icon
  - files: `Engine/Nodes/Scene/docs/USDLightLink.md`, `Gui/Resources/Images/NativeNodes/USDLightLink.png`
  - approach: docs in the style of the other scene nodes: every knob, rule types, prim-centric synthesis, row merging, include-root behaviour, the delegate table from M567.P1.T2 updated for Cycles; no reference-app names. Grouping `PLUGIN_GROUP_USD_Lights` (M520), or `PLUGIN_GROUP_3D_USD` if M520 has not landed.
  - verify: the node shows in the submenu; the docs name every knob and the table matches the constants.
  - size: M

- [ ] M567.P3.T6 — Params-to-knob audit
  - files: this milestone file (a checked table)
  - approach: walk the Light Linker control list in Scope against the built knobs; each In control has a knob and a doc line.
  - verify: the table in the PR body maps every control.
  - size: S

## Phase 567.4: End-to-end check

- [ ] M567.P4.T1 — Render check and Xvfb GUI script
  - files: `Tests/gui/usd_lightlink.py`, `Tests/Scene/USDLightLink_Test.cpp`
  - approach: build two spheres, a light and USDLightLink in the Storm viewer and in Cycles, MoonRay or Utility; add a second rule row, remove a row, edit `Excludes`, and assert the stage and that the viewer render changes (knob-driven refreshes); assert the warning text for a delegate the table says does not show links. The panel at 1920x1080 with five rows is screenshotted.
  - verify: the script passes under Xvfb at 1920x1080; the user OKs the screenshots.
  - size: M

**Verification gate:** release gate green per `.local/build-mode` with the bumped Cycles pin; `USDLightLinkDelegates_Test` and `USDLightLink_Test` pass and Cycles shows both link types; existing Cycles goldens are unchanged; `Tests/gui/usd_lightlink.py` passes under Xvfb at 1920x1080; the probe table in Decisions matches the docs; the user OKs the screenshots.

## Decisions

- 2026-11-24 — **Milestone, probe first (user):** Phase 1 probes which bundled delegates (Storm, Cycles, MoonRay, utility renderer) honour UsdLux `lightLink` and `shadowLink`; the node docs state which delegates show it.
- 2026-11-24 — **Rule structure is Solaris' (user):** several rule rows per node (Enabled, Light or light collection, Includes, Excludes, Type Light or Shadow, Reversed as prim-centric, synthesized into light-centric links) plus collection rows, on a plain knob panel. The three-column drag-and-drop panel is out; M588 owns it.
- 2026-11-24 — **Patch Cycles here (user):** the Cycles fork gets light and shadow linking in this milestone; Storm's result is recorded only.
- 2026-11-24 — The reusable row-list helper `KnobRowList` is built here (M567.P3.T1) rather than in M575 - USD Restructure And Graft, because light linking ships first; M575 depends on this milestone and reuses it. RFVSwitch is not migrated onto it (planner). Deps are M562 and M566.
