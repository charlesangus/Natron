# M447 - USDPreviewSurface Rename

Rename the `USDMaterial` node to `USDPreviewSurface`, its plugin ID and label both, because that is what it authors and M445 - USDOpenPBR Node adds a sibling for OpenPBR (user, 2026-11-15). The fork keeps no backward compatibility, so a project saved with `USDMaterial` loads the missing-plugin node. The rename lands on main on its own, ahead of F26 - MaterialX Rendering, to keep rebase churn small for every lane that touches scene nodes.

## Scope

**In:**
- `PLUGINID_NATRON_USDMATERIAL` "USDMaterial" becomes `PLUGINID_NATRON_USDPREVIEWSURFACE` "USDPreviewSurface"; class, files, `.reg.inc`, docs page and icon renamed to match.
- Every reference on main: the scene nodes' headers and docs that name it (`USDAssignMaterial`, `USDCard`, `USDCube`, `USDCylinder`, `USDSphere`, `USDEmissiveMaterial`, `USDProjectUV`, `USDWrite`), `Gui/SceneGraphModel.cpp`, tests, GUI scripts, Python tests and `docs/cache-invalidation/node-private.md`.
- The default node name and Prim Path follow the new name (`/USDPreviewSurface1`).

**Out:**
- Published decision records under `docs/decisions/`: history, left as written.
- Plan briefs that name `USDMaterial`: swept by `/cat-plan` once this merges (the gate files the request).

**Edge cases:**
- Text that means "a USD material" in general rather than this node: left alone. The rename touches the node name only.
- An open branch that adds a new reference: rebases onto this and renames it; the gate's grep runs on main after the merge.

## Phase 447.1: Rename

- [ ] M447.P1.T1 — Rename the node everywhere
  - files: `Engine/Nodes/Scene/USDMaterial.{h,cpp}` → `USDPreviewSurface.{h,cpp}`, `Engine/Nodes/Scene/USDMaterial.reg.inc` → `USDPreviewSurface.reg.inc`, `Engine/Nodes/Scene/docs/USDMaterial.md` → `USDPreviewSurface.md`, `Engine/CMakeLists.txt`, the other files in Scope **In**, `Tests/Scene/USDMaterial_Test.cpp` → `USDPreviewSurface_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: `git mv` the files, then replace the node's identifier, class name, plugin ID, label and doc links across every file `git grep -w USDMaterial` lists outside `docs/decisions/`. Check each hit is the node, not general prose. Update test expectations for the new default name and Prim Path.
  - verify: the build passes; `USDPreviewSurface_Test`, `USDAssignMaterial_Test`, `USDPrimitives_Test`, `USDWrite_Test`, `SceneIntegration_Test` and `USDRenderStorm_Test` pass; `git grep -w USDMaterial -- ':!docs/decisions'` returns nothing.
  - size: M

- [ ] M447.P1.T2 — Update GUI and Python checks
  - files: `Tests/gui/scene_assign_material.py`, `Tests/gui/scene_node_knob_layout.py`, `Tests/python/test_tabtabtab_natron.py`
  - approach: rename the node in each script and its expectations; the tab menu lists USDPreviewSurface and no USDMaterial.
  - verify: both GUI scripts pass under Xvfb and the Python test passes.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; the tests above pass; after merging, `git grep -w USDMaterial origin/main -- ':!docs/decisions'` is empty; the PM appends a `# Open questions` entry asking `/cat-plan` to sweep todo briefs that name `USDMaterial`. No UAT: the rename has no behaviour of its own.
