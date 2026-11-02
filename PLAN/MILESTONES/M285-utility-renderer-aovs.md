# M285 - Utility Renderer AOVs

Expose Natron Utility's full output set through Hydra AOV descriptors and deliver it to
USDRender through M256 - Delegate AOV Selection, of which this renderer is the first full
client: data AOVs (depth, position, normals, UV, motion vectors, ids, albedo, coverage, any
primvar), lighting lobes, light groups, and Cryptomatte. Once delivered, a new USDRender
defaults to Natron Utility.

## Scope

**In:**
- Data AOVs: `depth` (USDRender convention), `Zlinear`, `Zdistance`, `P` (world), `Peye`, `N` (shading, world), `Ng`, `Neye`, `uv`/`st`, `motionFwd` and `motionBwd` (screen-space pixels per frame, plus Nuke's Distance / Velocity / normalized modes as a setting), `primId`, `instanceId`, `albedo`, `coverage`, `primvars:<name>`.
- Lighting AOVs: diffuse, specular, reflection, refraction, emission, shadow (unshadowed − shadowed), each direct-lit as M281/M282 compute them, summing to `color`.
- Light-group AOVs: one per UsdLux `lightgroup` value (lights with no group go to a default group).
- Cryptomatte: object (prim path), material and asset (model root) levels, ranked ID/coverage pairs at a `Cryptomatte Depth` setting (default 6), with the standard name, hash, conversion and manifest metadata.
- Filter-aware accumulation: data AOVs use closest-sample or average per their kind (ids and depth never filtered across surfaces).
- USDRender default renderer becomes Natron Utility for newly created nodes; the 3D viewer stays on Storm.

**Out:**
- The AOV selection UI and the AOV→layer mapping mechanism — M256 - Delegate AOV Selection.
- A Cryptomatte keyer node — M290 - Native Cryptomatte Keyer.
- Deep versions of these AOVs — M286 - Utility Renderer Deep Output.
- Indirect lighting AOVs — none (no GI).

**Edge cases:**
- Unselected AOVs cost nothing: the shading stage only computes bound AOVs (bitmask).
- Motion vectors on a static frame or zero shutter: zero vectors (not missing).
- Pixels with no hit: depth +inf (USDRender's existing convention), ids 0, Cryptomatte empty.
- A primvar AOV naming a primvar a prim lacks: zero for that prim; no warning.
- Over 6 overlapping ids per pixel: the lowest-coverage ids beyond the depth are dropped, per the Cryptomatte spec.
- Existing projects whose USDRender saved Storm keep Storm; only new nodes default to Natron Utility.

**Assumptions:**
- M256 - Delegate AOV Selection settles a mechanism that can carry delegate-specific AOV names and per-AOV metadata (Cryptomatte manifests); if it can't, M285 is re-planned with the user.
- Natron images can carry per-layer metadata into EXR writes (the Cryptomatte manifest keys); confirm against the native Write's metadata support.

## Phase 285.1: AOV production

- [ ] M285.P1.T1 — AOV registry and bound-AOV mask
  - files: `Engine/Nodes/Scene/Utility/UtilityAovs.{h,cpp}`, `Engine/Nodes/Scene/Utility/UtilityRenderDelegate.cpp`, `Engine/Nodes/Scene/Utility/Kernels/Film.slang`, `Tests/Scene/NatronUtilityAovs_Test.cpp`
  - approach: one table of AOV names → `HdAovDescriptor` (format, clear value, filter kind); `GetDefaultAovDescriptor` serves it; the render pass turns bindings into a bitmask the kernels test. Film gains per-kind accumulation (filtered sum, closest-sample, min).
  - verify: gtest — every AOV name returns a descriptor; binding only `color` runs no data-AOV writes (counter); binding an unknown name is rejected with a warning.
  - size: M

- [ ] M285.P1.T2 — Data AOVs
  - files: `Engine/Nodes/Scene/Utility/Kernels/ShadeSurface.slang`, `Engine/Nodes/Scene/Utility/Kernels/AovData.slang`, `Engine/Nodes/Scene/Utility/UtilityScene.cpp`, `Tests/Scene/NatronUtilityAovs_Test.cpp`
  - approach: write the data AOVs at the primary hit; motion vectors project the hit's object-space point at shutter open/close (or ±1 frame per mode) through the camera at those times using M283's motion samples.
  - verify: gtest — a cube translating 10 px per frame gives motionFwd (10, 0) on its pixels and 0 on the static background; P and N match analytic values on a unit sphere; primvar AOV returns an authored constant.
  - size: L

- [ ] M285.P1.T3 — Lighting lobes and light groups
  - files: `Engine/Nodes/Scene/Utility/Kernels/ShadeSurface.slang`, `Engine/Nodes/Scene/Utility/Kernels/Lights.slang`, `Engine/Nodes/Scene/Utility/UtilityLight.cpp`, `Tests/Scene/NatronUtilityAovs_Test.cpp`
  - approach: split each contribution by lobe and by its light's group into separate accumulators; group AOV names are discovered from the scene's lights and published as descriptors on sync.
  - verify: gtest — the lobe AOVs sum to `color` within 1e-4; two lights in groups `key` and `fill` produce two AOVs summing to the direct lighting.
  - size: L

- [ ] M285.P1.T4 — Cryptomatte
  - files: `Engine/Nodes/Scene/Utility/UtilityCryptomatte.{h,cpp}`, `Engine/Nodes/Scene/Utility/Kernels/Cryptomatte.slang`, `Engine/Nodes/Scene/Utility/UtilityAovs.cpp`, `Tests/Scene/NatronUtilityAovs_Test.cpp`
  - approach: per camera sample record (id, filter weight) for each level; per pixel keep the top-N by accumulated coverage (fixed-size per-pixel table on both backends); write ranked pairs as Cryptomatte layers; the manifest (name → MurmurHash3 float, the same `primPathHash` as primId) is published as AOV metadata.
  - verify: gtest — two half-covering prims in one pixel give two ranks with coverage ≈0.5 each; the manifest maps both names to their hash values; output decodes with the reference Cryptomatte Python decoder on a written EXR.
  - size: L

## Phase 285.2: Delivery

- [ ] M285.P2.T1 — Natron Utility AOVs through M256's mechanism
  - files: `Engine/Nodes/Scene/USDRender.cpp`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/docs/USDRender.md`, `Tests/Scene/USDRender_Test.cpp`
  - approach: implement whatever M256 - Delegate AOV Selection decided for this delegate's full list, including metadata pass-through for Cryptomatte and the motion-vector mode setting; document the AOV list.
  - verify: gtest — selecting `N`, `motionFwd` and `CryptoObject` on USDRender produces those Natron layers with the right channel names and Cryptomatte metadata.
  - size: M

- [ ] M285.P2.T2 — Natron Utility as the USDRender default
  - files: `Engine/Nodes/Scene/USDRender.cpp`, `Tests/Scene/USDRender_Test.cpp`, `Engine/Nodes/Scene/docs/USDRender.md`
  - approach: new USDRender nodes default the Renderer knob to Natron Utility when it is loadable (Storm otherwise); a saved value is never changed. The viewer default stays Storm.
  - verify: gtest — a new node's Renderer is Natron Utility; a project saved with Storm reloads as Storm; with the delegate forced unavailable a new node gets Storm.
  - size: M

**Verification gate:** full build and ctest green on both backends; a multi-AOV render is written to EXR and opened in the viewer per layer under Xvfb, screenshots shared with the user.

## Decisions

- 2026-11-01 — **Lobes plus light groups (user).**
- 2026-11-01 — **Cryptomatte in this milestone (user);** the keyer is M290 - Native Cryptomatte Keyer.
- 2026-11-01 — **AOVs go through M256 - Delegate AOV Selection (user):** no interim per-AOV toggles.
- 2026-11-01 — **New USDRender nodes default to Natron Utility (user)** once this milestone lands; the viewer stays Storm.
