# M368 - Native Instance Material Binding

Projection reaches native instanced prims through a direct material binding on their instance roots. USDAssignMaterial authors only a collection binding from an ancestor (`Engine/Nodes/Scene/USDAssignMaterial.cpp` ~:447-480 via `authorCollectionBinding`), which Storm does not carry onto native instances (lit grey in the M341 spike). So M341 - Per-Placement Geometry Projection and M342 - Sticky Projections On Deforming Geometry leave instanced matches unprojected and rigid, with a warning. Follow-up requested under F9 - Delegate Projection (user, 2026-11-07).

## Scope

**In:**
- USDAssignMaterial authors a direct `material:binding` on each native instance root its matches reach, alongside the collection binding.
- The per-placement projection frame primvar on instance roots, so each instance projects from its own placement.
- The M341 and M342 instance warnings and node docs updated to what now works.

**Out:**
- De-instancing: never done behind the user's back (M342 decision, 2026-11-05).
- Point instancer instances, which share their prototype's one placement as M341 records.
- Cycles and MoonRay projection on instances: M344 - Camera Projection In Delegates picks it up from the same binding.

**Edge cases:**
- A match inside a prototype but not at an instance root: bind at the nearest instance root above it, and warn when other prims under that root would pick up the material too.
- Sticky projection on a deforming instanced mesh: instances share the prototype's points, so the reference primvar can only be constant per instance; P1.T1 says whether that works, and if not it stays rigid with the existing warning.
- An instance root that already has a direct binding: the authored one wins as USD's strength rules say, and the node warns.

## Phase 368.1: Spike

- [x] M368.P1.T1 — Prove a direct binding and primvar reach Storm instances
  - files: `tools/spikes/storm-projection/` (`gen.py`, a new `run-instances.sh`), `## Decisions` here
  - approach: generate a scene with two native instances of one card, bind the projection material directly on each instance root with a different `natronProjectionFrame` primvar on each, and render through Storm under llvmpipe. Also try a held reference-points primvar on an instance root for the deforming case.
  - verify: each instance shows its own projection; the result for the deforming case is recorded in `## Decisions`.
  - size: M

## Phase 368.2: Author on instance roots

- [ ] M368.P2.T1 — Bind materials directly on instance roots
  - files: `Engine/Nodes/Scene/USDAssignMaterial.cpp`, `Engine/Nodes/Scene/SceneNodeAuthoring.cpp` (`authorMaterialBinding` ~:720), `Tests/Scene/USDAssignMaterial_Test.cpp`
  - approach: for each match that is an instance proxy, find its instance root and author a direct binding there with `authorMaterialBinding`, keeping the collection binding for the rest.
  - verify: `USDAssignMaterial_Test`: a match under a native instance gets a direct binding on the instance root and `ComputeBoundMaterial` resolves it for the proxy; a non-instanced match is unchanged.
  - size: M

- [ ] M368.P2.T2 — Author projection primvars on instance roots
  - files: `Engine/Nodes/Scene/SceneNodeAuthoring.cpp` (`authorProjectionGeometryFrame`), `Engine/Nodes/Scene/SceneProjectionCameras.cpp`, `Engine/Nodes/Scene/USDWrite.cpp` (`eReasonInstanced`), `Tests/Scene/USDProject3D_Test.cpp`
  - approach: author the placement frame primvar, and the sticky reference primvar where P1.T1 showed it works, on each instance root. Drop the instance warning where projection now reaches the prim and keep it where it doesn't. USDWrite bakes instanced projections it can now see.
  - verify: `USDProject3D_Test`: two instances of one prim get their own frames on their instance roots; instanced sticky projections keep their warning.
  - size: M

- [ ] M368.P2.T3 — Update the projection node docs
  - files: the USDAssignMaterial and projection node docs touched by M341 - Per-Placement Geometry Projection and M342 - Sticky Projections On Deforming Geometry
  - approach: replace the instance limit text with what now works and what still doesn't.
  - verify: the docs name native instances as supported and keep the point-instancer limit.
  - size: S

**Verification gate:** `USDAssignMaterial_Test`, `USDProject3D_Test` and `SceneMaterialBinding_Test` pass, and the spike renders each instance with its own projection. A Storm screenshot of two projected instances is collected for F9 - Delegate Projection's UAT.

## Decisions

- 2026-11-07 — A direct binding on native instance roots, as a follow-up in F9 - Delegate Projection (user): collection bindings from an ancestor don't reach native instances in Storm.
- 2026-11-12 — **P1.T1 spike (258786d08, `run-instances.sh`):** Storm honours a direct `material:binding` on each native instance root (projection shader, not lit grey), and a constant `natronProjectionFrame` matrix primvar on the root reaches the shader per instance, so each instance projects from its own frame. A constant `natronReferencePoints` on the root is read per instance too, but it is one point, not per-vertex: instances share prototype points, so per-vertex sticky reference points can only live in the prototype, shared by every instance. P2.T2 therefore authors the frame primvar per instance root, and keeps sticky projection on instances rigid with the existing warning unless all instances of a prototype share one reference pose.
- 2026-11-12 — P2.T1 implemented (b03bcdb95): proxy matches bind at their instance root and drop out of the collection binding; ancestor/root matches also bind each instance root below; `SceneOps::selectPrims` gained `traverseInstanceProxies`. Tests run with the phase build.
- 2026-11-12 — **USDWrite keeps `eReasonInstanced` for instanced projections (PM, engineering call):** the bake authors `primvars:st` through overs, which can't reach below an instance, and instances share their prototype's UVs while each projects from its own frame, so a bake would silently lose the projection. Instances keep the Storm shader and the warning on export; de-instancing is out of scope. P2.T2's USDWrite verify line is dropped accordingly. P2.T2 implemented in 28bb0d138: frames go on each bound instance root; a gprim offset from its root projects shifted, with a warning; instanced sticky projection stays rigid, with a warning.
