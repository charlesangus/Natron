# M159 - Kind-Agnostic Groups

Groups, and the PyPlug instances that are groups, stop being an image barrier. A group node resolves each external input's kind from what its GroupInput feeds inside, and its output kind from what feeds GroupOutput. This works for every kind: image, deep, scene and raw. Today the inner GroupInput and GroupOutput are already polymorphic (`NoOpBase`, `Engine/NoOpBase.h:65-73`), and GroupInput already resolves upstream through the group's real input (`Engine/Node.cpp:2050-2063`). Rendering already passes through groups: `Node::getInput` applies group redirection (`Engine/NodeInputs.cpp:238`), and the deep render path has no group checks. What remains is three things. The group node itself takes `EffectInstance`'s image/image default (`EffectInstance.h:524,532`; `NodeGroup.h:309` has no override). Nothing follows GroupOutput's input. And the downstream and conflict walks (`Node.cpp:2070`, `:2123`) don't cross the group boundary.

## Scope

**In:**
- `NodeGroup` declares polymorphic inputs and output, and the output resolves from GroupOutput's upstream.
- The downstream-requirement and conflict walks cross group boundaries in both directions.
- Cache invalidation crosses the boundary.
- PyPlug instances and nested groups work.
- Project load reports conflicts through groups.
- The README's "Known limitation: groups are an image barrier" section is replaced.

**Out:**
- Moving Develop RAW from a backdrop to a Group: none (M80 - Native RAW Support keeps the backdrop by decision).
- Explicit per-GroupInput/GroupOutput kind knobs: none (inference only).
- Storing kinds in serialization: none, because kinds stay derived on load.

**Edge cases:**
- A group whose GroupOutput is unconnected resolves as an unfed polymorphic node does today (the existing ambiguity rules).
- A group input connected outside but unused inside resolves from the external input alone.
- A group input unconnected outside resolves from what its GroupInput's consumers inside require (the downstream walk).
- Nested groups resolve recursively under the existing thread-local cycle guard (`Node.cpp:1845-1870`).
- Several group inputs of different kinds resolve independently.

**Assumptions:**
- `renderDeepRoI` is never called directly on a group node, only on what `getInput` redirects to (`EffectInstanceRenderDeep.cpp:405`). P1.T1 confirms this.

## Phase 159.1: Group kind resolution

- [ ] M159.P1.T1 — The group node's output kind follows GroupOutput
  - files: `Engine/NodeGroup.h/.cpp`, `Engine/Node.cpp` (`getEffectiveOutputDataKind` ~1908, `resolveStructuralOutputDataKind` ~2005, `collectUpstreamDataKindConstraint` ~2030), new `Tests/DataKindGroup_Test.cpp`
  - approach: override `getOutputDataKind`/`getInputDataKind` on `NodeGroup` to return `eDataKindPolymorphic`. In the upstream constraint walk, a `NodeGroup` resolves through `getOutputNodeInput()` (`NodeGroup.cpp:1418`): it merges the effective kind of what feeds its GroupOutput. Keep the cycle guard. Confirm that `renderDeepRoI` and `producesDeepData` (`EffectInstanceRenderDeep.cpp:205-216,405`) never see the group node itself, and handle it if they do.
  - verify: gtest: a deep source → group (GroupInput → deep node → GroupOutput) → deep consumer connects, resolves deep at the group's output and renders the same samples as without the group; the same for an image chain (unchanged behaviour); an unconnected GroupOutput follows the existing unfed-polymorphic rules.
  - size: L

- [ ] M159.P1.T2 — Downstream requirements and conflicts cross the boundary
  - files: `Engine/Node.cpp` (`collectDownstreamDataKindRequirement` ~2070, `findDataKindConflictDownstream` ~2123, `isInputDataKindUnacceptable` ~2172), `Tests/DataKindGroup_Test.cpp`
  - approach: when the downstream walk reaches a group node's input, continue into the matching GroupInput's consumers inside. When it reaches a GroupOutput, continue to the group node's outputs (`getOutputsWithGroupRedirection`). Conflict detection follows the same path, so a raw→image mismatch inside a group is reported at the external edge that causes it.
  - verify: gtest: an unconnected group input whose inner consumer is deep-only resolves deep; connecting an image source to it is rejected with `eCanConnectInput_incompatibleDataKind`; a mismatch inside a group is reported on the right edge.
  - size: M

- [ ] M159.P1.T3 — Invalidation across the boundary
  - files: `Engine/Node.cpp` (`invalidateEffectiveOutputDataKindCache` ~2211), `Engine/NodeInputs.cpp` (`onInputChanged` ~1568, group branch ~1639), `Tests/DataKindGroup_Test.cpp`
  - approach: a change to GroupOutput's input invalidates the enclosing group node's cache and walks on to its outputs. The existing group-input branch (`NodeInputs.cpp:1639`) stays as it is. Nested groups propagate both ways.
  - verify: gtest: swapping the node feeding GroupOutput from image to deep flips the group's resolved kind and re-reports downstream conflicts; the same one level deeper in a nested group.
  - size: M

- [ ] M159.P1.T4 — PyPlugs and project load
  - files: `Tests/DataKindProjectLoad_Test.cpp`, `Tests/PyPlugInstantiate_Test.cpp`, `Engine/Project.cpp` (`reportDataKindConflicts` ~445, only if the walk needs it)
  - approach: a PyPlug whose inner chain is deep instantiates with a deep output kind and connects to a deep consumer. A saved project with a deep chain inside a group reloads with the same kinds and no spurious conflicts, and a genuinely conflicting one reports on load. Kinds are not serialized.
  - verify: the new load and PyPlug cases pass; the existing `DataKind*` and `DeepNodes_Test` cases are unchanged.
  - size: M

- [ ] M159.P1.T5 — README and edge colour check
  - files: `Engine/Nodes/README.md` (replace "Known limitation: groups are an image barrier"), new `Tests/gui/m159_group_kinds.py`
  - approach: document how groups resolve kinds and the edge cases above. An Xvfb script builds a group around a deep chain and screenshots the group's external edges, which should be coloured deep. Share the screenshot with the user (project rule).
  - verify: the script passes under Xvfb; the screenshot is shared and the user approves it.
  - size: M

**Verification gate:** `ctest` is green, including `DataKindGroup_Test` and the extended `DataKindProjectLoad`/`PyPlugInstantiate` cases, with `DataKind_Test` and `DeepNodes_Test` unchanged; the Xvfb script passes and the user has approved the screenshot.
