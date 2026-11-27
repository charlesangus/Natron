# M632 - Linear Input Data Refresh

Input-data refresh scales badly on long chains. `AppManager::clearAllCaches` → `forceComputeInputDependentDataOnAllTrees` recurses about 80 frames of `refreshInputRelatedDataInternal` (`Engine/Node.cpp:10488`, which recurses before marking; RotoPaint re-dirties at :10524-10527) down a 70-item Merge chain. So `Operators/RotoPaintDirectCompositeTest` (added by M299 - Roto Render Performance) takes 9–38 s per case under -j4, since it calls `CacheStress::clearEverything` before every render. M299 runs that test serially as a stopgap. Profile it and make the refresh linear in the graph size. This is related to the O(N²) upstream walks in the 2026-09-25 render-scaling findings. `getUpstreamInputEditStamp` (:10584) is already iterative and memoized, so it is not the cause.

## Scope

**In:**
- `forceComputeInputDependentDataOnAllTrees` and `refreshInputRelatedDataInternal`: each node refreshed once per pass.
- Removing M299's serial stopgap for the test once it is fast.

**Out:**
- Other upstream walks from the render-scaling findings, unless the profile shows they dominate this path.

**Edge cases:**
- **Diamond graphs:** a node reached by two paths is refreshed once, after all of its inputs.
- **RotoPaint re-dirty:** the re-dirty at :10524 must still take effect, so mark-then-refresh in topological order, not mark-and-skip.

## Phase 632.1: Profile and fix

- [ ] M632.P1.T1 — Profile the refresh on a 70-Merge chain and a wide diamond graph
  - files: read-only: `Engine/Node.cpp` (:10488-:10600), `Engine/AppManager.cpp` (`clearAllCaches`), `Tests/GraphScalingConnect_Test.cpp` (:225, :411, :454); notes to `build/m632/profile.md`
  - approach: time the refresh at chain lengths 10/35/70/140 and count `refreshInputRelatedDataInternal` calls per node. Get stacks per the profiling recipe in the container (eu-stack; no perf). Identify which recursion revisits nodes.
  - verify: profile.md shows calls-per-node and time versus N, and names the super-linear site
  - size: M
- [ ] M632.P1.T2 — Refresh each node once, in topological order
  - files: `Engine/Node.cpp` (`refreshInputRelatedDataInternal`, `forceComputeInputDependentDataOnAllTrees`), `Engine/AppManager.cpp`; `Tests/GraphScalingConnect_Test.cpp`
  - approach: compute one topological order of the affected trees with a visited set, then refresh each node once, so RotoPaint's re-dirty is seen by its consumers in the same pass. Add a scaling test asserting calls-per-node is 1 and that time at N=140 is under ~2.5x the time at N=70 (a loose bound against regression, not a benchmark).
  - verify: the new scaling test passes; `ctest -R "GraphScaling|RotoPaint|Node"` green; RotoPaintDirectCompositeTest cases each under 2 s locally
  - size: L
- [ ] M632.P1.T3 — Drop M299's serial stopgap for RotoPaintDirectCompositeTest
  - files: `Tests/SuiteProperties.cmake` or wherever M299 serialised the test (find `RotoPaintDirectComposite` RUN_SERIAL)
  - approach: remove the RUN_SERIAL / serial-list entry M299 added.
  - verify: `ctest -j4 -R RotoPaintDirectComposite` passes in under a minute
  - size: S

**Verification gate:** full build and ctest green at -j4; GUI bench (Engine/Node* rule) shows no regression.
