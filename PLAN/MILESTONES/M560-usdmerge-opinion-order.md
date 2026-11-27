# M560 - USDMerge Opinion Order

USDMerge gets a setting choosing whether lower- or higher-numbered inputs win when opinions conflict. The use case is USDRead→USDToGeo→[geometry work]→GeoToUSD→USDXformPrim (parent it in place)→USDMerge back into the scene, without the new branch's xforms stomping the existing ones. USDXformPrim's scene input covers xforms only and isn't always convenient (user, 2026-11-23).

Today `USDMerge::mergeStacks` (`Engine/Nodes/Scene/USDMerge.cpp:53-95`) concatenates each input's layers, weakest first (`SceneOps::getLayers`, `SceneOps.h:340`), in input order 0..63 (`:98-110`). A layer shared across inputs stays at its first, weakest position, so higher-numbered inputs always win. That matches the merge-node convention and the `TheLastInputWinsAConflictingAttribute` test (`Tests/Scene/USDMergePrune_Test.cpp:281`). The new `opinionOrder` setting can reverse the strength order. Lower Inputs Win walks the inputs from highest to lowest and keeps the same dedup rule, so a diamond's shared source still sits under both branches' edits. It is a layer-stack union only: no flattening, no references.

## Scope

**In:**
- the `opinionOrder` knob;
- the reversed order in `mergeStacks`;
- tests;
- updated docs and description.

**Out:**
- per-attribute or per-input strength;
- filtering xforms out of a branch;
- flatten modes.

**Edge cases:**
- Under Lower Inputs Win, these all follow the new strength order, so input 0's opinions are strongest:
  - stage metadata (default prim, up axis, time codes);
  - prepend/append list-op order (apiSchemas, references);
  - the op-order union for an Append transform below the merge.
- The prefix sharing (`USDMerge.cpp:82-90`) then starts from the highest-numbered non-empty input.
- One input, or no input: unchanged under both orders.
- The user's case:
  - an existing ancestor's xform beats the new branch's, and the new prims still come through;
  - points that both branches author also take input 0's value (documented).

## Phase 560.1: Order

- [ ] M560.P1.T1 — Reversible strength in mergeStacks
  - files: `Engine/Nodes/Scene/USDMerge.{h,cpp}`, `Tests/Scene/USDMergePrune_Test.cpp`
  - approach: `mergeStacks(inputs, bool lowerWins = false)` iterates the inputs in reverse when `lowerWins` is set, keeping the weakest-position dedup and the prefix reuse. Update the doc comment at `USDMerge.h:84-89`.
  - verify: gtest calling `mergeStacks` directly:
    - a conflicting attribute takes input 0's value;
    - a diamond keeps its shared layer once, under both branches;
    - a single input passes through unchanged under both orders.
  - size: M
- [ ] M560.P1.T2 — The Opinion Order knob
  - files: `Engine/Nodes/Scene/USDMerge.{h,cpp}`, `Tests/Scene/USDMergePrune_Test.cpp`
  - approach: `initializeKnobs` creates KnobChoice `opinionOrder`, labelled "Opinion Order", with choices "Higher Inputs Win" (the default) and "Lower Inputs Win", following `USDEditPrim.cpp:161-227`. `renderScene` passes the knob value through. Tooltip: conflicting opinions follow this order; an opinion only one input authors always comes through.
  - verify: node-level gtest:
    - the user's case (source /Group xform; a branch adding /Group/New and authoring /Group's xform) keeps the source xform and contains /Group/New;
    - an Append transform below the merge orders its ops by the new strength;
    - changing the knob recooks to a different stage.
  - size: M
- [ ] M560.P1.T3 — Docs and description
  - files: `Engine/Nodes/Scene/docs/USDMerge.md`, `USDMerge.h:62`
  - approach: describe both orders and the edge cases above. Remove the Houdini mention at `USDMerge.md:8`, since docs never name it.
  - verify: the docs build, and `rg -i houdini Engine/Nodes/Scene/docs/USDMerge.md` finds nothing.
  - size: S

**Verification gate:**
- build and ctest green on both legs, including USDMergePrune;
- an Xvfb check of the Opinion Order knob on USDMerge;
- a UAT AppImage with a steps .md.

## Decisions
- 2026-11-23 — Requested by the user during F15 UAT; general, not an F15 blocker.
- 2026-11-26 (PM) — One global choice, as the user requested. A "strongest input" picker and per-branch xform exclusion stay out.
