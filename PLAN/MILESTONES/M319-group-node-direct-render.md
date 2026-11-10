# M319 - Group Node Direct Render

Rendering a Group node directly returns the pixels of the node feeding its Output, in both schedulers, or a clean empty result when nothing feeds it — never unwritten memory. Found by M121 - Native Routing And Mix: `NodeGroup` (Engine/NodeGroup.h ~297-415) overrides none of `isIdentity`, `render`, `getRegionOfDefinition` or metadata, and the base `render` returns `eStatusOK` without writing. Downstream nodes and the Viewer are unaffected because `Node::getInput` redirects through `applyNodeRedirectionsUpstream` (Engine/NodeInputs.cpp ~80-112, 263) to the Output's input; only callers that render the group itself see the bug. Identity consumers resolve the source with `getInput(identityInputNb)`, which for a group means its external inputs, so a new identity seam is needed (EffectInstanceRenderRoI.cpp ~138/167, ~671; ParallelRenderArgs.cpp ~1068; EffectInstance.cpp ~950; RenderScheduler.cpp ~541-572 copies request-pass deps). The render-state walk (ParallelRenderArgs.cpp ~1984-2033) follows only `getInput(i)`, and `Node::getHashInputs` (Node.cpp ~1551-1591) leaves out the group's interior; the hooks `appendExtraHashSources`/`appendExtraHashDependents` exist.

## Scope

**In:**
- NodeGroup identity through one engine seam; the group hash follows its interior; RoD, metadata and frame range mirror the source.
- Drop the Group Dot workarounds; direct-render tests in both scheduler modes.

**Out:**
- PrecompNode (its output lives in another app; keep `consumerOf`'s Precomp branch).
- The CacheStress grouped-chain Dot (a real consumer).
- The Viewer (already redirected).

**Edge cases:**
- No Output, or nothing feeds it: identity with input -1, Ok with no layers, like an unconnected Dot.
- Output fed by a GroupInput: resolves to the external input.
- Nested groups; a generator inside a group with no inputs (RoD is the generator's); deep content; PyPlug and Tracker groups (both are NodeGroups); an inner edit after a render (no stale cache).

## Phase 319.1: Group as identity

- [x] M319.P1.T1 — The group's hash folds in what feeds its Output
  - files: `Engine/NodeGroup.{h,cpp}`, `Engine/GroupOutput.{h,cpp}`, `Tests/GroupDirectRender_Test.cpp` (new)
  - approach: `NodeGroup::appendExtraHashSources` adds `getOutputNodeInput(false)`; `GroupOutput::appendExtraHashDependents` adds the containing group's node.
  - verify: the new test shows the group's hash changes when an inner knob or the Output's input changes.
  - size: M
- [x] M319.P1.T2 — Internal identity source, used by NodeGroup
  - files: `Engine/EffectInstance.{h,cpp}`, `Engine/EffectInstanceRenderRoI.cpp`, `Engine/ParallelRenderArgs.cpp`, `Engine/NodeGroup.{h,cpp}`, `Tests/GroupDirectRender_Test.cpp`
  - approach: add a sentinel identity input number (e.g. -3) and `EffectInstance::getIdentitySource(int nb)` (nb≥0 → `getInput`; sentinel → virtual, null by default); replace `getInput(identityNb)` at the identity sites above; make the render-state walk visit the source. `NodeGroup::isIdentity` returns the sentinel when the Output is fed, -1 otherwise. RoD, preferred metadata and frame range come from the source; no layer pass-through to external inputs (confirm the pass-through step before the identity check, EffectInstanceRenderRoI.cpp up to ~590 and ParallelRenderArgs.cpp ~107, doesn't route planes to external input 0). Grep for `identityInputNb >= 0` assumptions; the sentinel must not reach OFX code.
  - verify: a group rendered directly with `renderNodePlanesInMode` in Legacy and Task-graph matches, bit for bit, the same render through a Dot below, over 3 runs with caches cleared after a different-colour render; a generator-only group has the right RoD; an unfed group gives no plane and an Ok code.
  - size: L

## Phase 319.2: Drop workarounds, edge coverage

- [x] M319.P2.T1 — Render groups directly in existing tests
  - files: `Tests/Native/CacheEquivalence_Test.cpp`, `Tests/Native/LightWrapRender_Test.cpp`
  - approach: `consumerOf` keeps only its Precomp branch; LightWrap renders `_wrap` and drops `_out`; the Dot4 check becomes `getOutputNodeInput(false) == inner("Dot4")`.
  - verify: both tests pass in both modes; the sweep reports Group and PyPlugs as compared, not skipped.
  - size: M
- [ ] M319.P2.T2 — Edge-case direct renders
  - files: `Tests/GroupDirectRender_Test.cpp`
  - approach: nested group, deep chain in a group, Output fed by a GroupInput, inner edit after a render (no stale RoD or pixels).
  - verify: each case matches its Dot-below reference in both modes.
  - size: M

**Verification gate:** GroupDirectRender_Test, DataKindGroup_Test, CacheStress_Test and the full ctest pass in both scheduler modes; no Group Dot left in the two tests; PR CI (debug asserts) green.

## Decisions

- 2026-11-10 — Elaborated at claim (consultant). An unfed group renders as empty Ok, matching an unconnected Dot (a one-line change in P1.T2 if a hard failure is wanted). PrecompNode has the same gap but is out of scope.
- 2026-11-10 — P1.T2: the sentinel is -4 (kIdentityInternalSource), because -3 is already TaskEdge::kContextRequestInputNb. Deep and scene renders take the same seam.
