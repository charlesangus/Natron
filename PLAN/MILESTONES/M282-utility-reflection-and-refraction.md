# M282 - Utility Reflection And Refraction

Add secondary rays to Natron Utility: mirror and glossy reflection from UsdPreviewSurface's
specular lobe, refraction through transmissive surfaces (opacity < 1 with an IOR), Fresnel,
per-type ray depth limits, the Dome light as the environment on a miss, and tinted shadows
through transmissive surfaces. No GI and no caustics.

## Scope

**In:**
- Reflection rays from the specular lobe (mirror at roughness 0, GGX-sampled glossy above), weighted by Fresnel per the PreviewSurface spec; clearcoat reflects too.
- Refraction rays for transmissive surfaces (opacity < 1 and ior ≠ 1, per the UsdPreviewSurface spec); opacity with ior = 1 is plain transparency (the ray continues straight).
- Render settings `Max Ray Depth` (total, default 10), `Reflection Depth` (default 1), `Refraction Depth` (default 2), matching Nuke ScanlineRender's defaults.
- A ray that escapes returns the Dome light's radiance, or black with no Dome.
- Shadow rays through transmissive or partly opaque surfaces continue, attenuated by transmission colour and opacity (user, 2026-11-01).
- Secondary hits are shaded with M281's direct lighting (shadows included).

**Out:**
- Indirect diffuse, caustics, dispersion, thin-film, subsurface — none.
- Reflection/refraction AOVs — M285 - Utility Renderer AOVs.

**Edge cases:**
- Total internal reflection: the refraction ray reflects; counts against refraction depth.
- Depth limit reached: the path terminates with the Dome contribution excluded (black), not the surface colour, so energy loss is visible and predictable.
- Back-facing hit inside a closed glass object: IOR inverted for exit; open meshes treat each face as an interface (documented).
- Reflection depth 0: no reflection rays, specular from lights only.
- Rough reflections are noisy until enough pixel samples: they converge with M283 - Utility Renderer Sampling And Motion Blur's samples.

**Assumptions:**
- The UsdPreviewSurface spec's treatment of `opacity` and `ior` (refraction vs. presence) reads as stated in Scope; confirm against the spec in `.local/docs/usd` before building.

## Phase 282.1: Secondary rays

- [ ] M282.P1.T1 — Bounce queue in the wavefront
  - files: `Engine/Nodes/Scene/Utility/UtilityRenderPass.cpp`, `Engine/Nodes/Scene/Utility/WavefrontCpu.cpp`, `Engine/Nodes/Scene/Utility/WavefrontGpu.cpp`, `Engine/Nodes/Scene/Utility/Kernels/UtilityTypes.slang`, `Tests/Scene/NatronUtility_Test.cpp`
  - approach: the shading stage may emit continuation rays (with throughput, depth counters per type and pixel index) into a next-bounce queue; the executor loops intersect → shade until the queue is empty or the total depth is reached; compaction between bounces. Register the three depth settings as `HdRenderSettingDescriptor`s.
  - verify: gtest — a shader stub that always emits one continuation ray terminates at the configured total depth; with depth 0 no second bounce runs (counter); both executors agree.
  - size: L

- [ ] M282.P1.T2 — Reflection and environment on miss
  - files: `Engine/Nodes/Scene/Utility/Kernels/ShadeSurface.slang`, `Engine/Nodes/Scene/Utility/Kernels/PreviewSurface.slang`, `Engine/Nodes/Scene/Utility/Kernels/Lights.slang`, `Tests/Scene/NatronUtilityReflect_Test.cpp`
  - approach: sample the specular (and clearcoat) lobe with GGX VNDF, Fresnel-weight it and emit a reflection ray; misses look up the Dome radiance.
  - verify: gtest — a mirror plane under a constant-colour Dome returns that colour × Fresnel at the expected angle; a red sphere's reflection appears in a mirror plane at the analytic position; roughness 0.5 converges to the reference mean with 256 samples.
  - size: L

- [ ] M282.P1.T3 — Refraction and transmissive shadows
  - files: `Engine/Nodes/Scene/Utility/Kernels/ShadeSurface.slang`, `Engine/Nodes/Scene/Utility/Kernels/PreviewSurface.slang`, `Engine/Nodes/Scene/Utility/Kernels/ShadeShadow.slang`, `Tests/Scene/NatronUtilityReflect_Test.cpp`
  - approach: transmissive hits emit a refraction ray (Snell, TIR fallback, entering/exiting by facing); ior = 1 transparency continues straight. Shadow rays become any-hit loops that accumulate transmission instead of a boolean occlusion — on GPU a `RayQuery` candidate loop, on CPU Embree's occlusion filter callback.
  - verify: gtest — a glass slab (ior 1.5) offsets a background stripe by the analytic amount; TIR occurs past the critical angle; a 50% transmissive red pane casts a red-tinted half-intensity shadow on both backends.
  - size: L

**Verification gate:** full build and ctest green on both backends; a GUI render of a reflective and refractive test scene under Xvfb is shared with the user.

## Decisions

- 2026-11-01 — **Ray-traced reflection and refraction (user).**
- 2026-11-01 — **Tinted transparent shadows, no caustics (user).**
- 2026-11-01 — **Depends on sampling:** glossy lobes need M283 - Utility Renderer Sampling And Motion Blur's pixel samples to converge.
