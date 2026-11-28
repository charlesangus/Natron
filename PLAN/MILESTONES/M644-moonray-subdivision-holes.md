# M644 - MoonRay Subdivision Holes

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

MoonRay renders the hole faces of USD subdivision meshes as solid faces. M554 - Scene Camera For Delegate Renders taught hdMoonray to drop hole faces on polygon meshes, but three gaps remain for subdivision surfaces. First, MoonRay's core ignores holes: `lib/rendering/geom/prim/OpenSubdivMesh.cc` (~:428) hard-codes `ignoreHoles = true`, so `desc.numHoles` is always 0 even though `SubdMesh` already carries an `mHoleIndices` buffer. Second, the RDL mesh hdMoonray fills in has no hole attribute. Third, hdMoonray never reads `GetHoleIndices` for subdivision meshes. The fix is a patch to the openmoonray fork: add an RDL hole attribute, feed it into `SubdMesh`'s hole indices, set `ignoreHoles` to false and confirm the code downstream handles holes, as the comment beside it doubts, then have hdMoonray fill the attribute in. Pin the result in Natron and add a render test.

Blocked on: M554 - Scene Camera For Delegate Renders reaching main with F1 - VDB Volumes. This patch must branch from M554's final openmoonray and hdMoonray pins rather than start a second fork line, and its brief can't name that base until F1 lands.

Acceptance sketch:
- A subdivision mesh with authored holes renders through MoonRay with those faces open, matching Storm and Cycles.
- Polygon-mesh holes (M554) still work, and meshes without holes render as before.
- One fork branch, built from M554's pins, pinned in Natron, with a MoonRay render test in ctest.

## Scope

**Out:**
- Cycles hole and boundary handling (done in M554 - Scene Camera For Delegate Renders).
- Other OpenSubdivMesh options (`useStudioDefault*`).

**Assumptions:**
- OpenSubdiv's refiner handles `holeIndices` correctly once MoonRay passes them; MoonRay's own tessellation downstream is what needs checking.
