## Pending

### 2026-11-27 — change-request — MoonRay subdivision surfaces ignore holes
- from: bosley-executor (cat-pm)
- refs: M554
- body: hdMoonray now drops hole faces from polygon meshes (M554 - Scene Camera For Delegate Renders), but MoonRay's core ignores holes on subdivision surfaces: `OpenSubdivMesh.cc` (~:428) hard-codes `ignoreHoles = true` and `RdlMeshGeometry` has no hole attribute. Fix needs a MoonRay core change in the fork (an RDL hole attribute fed from hdMoonray, honoured by OpenSubdivMesh). Small fork milestone, or fold into the next MoonRay fork bump.
