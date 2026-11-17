# M470 - LensFlare From USD Lights

Let LensFlare take its sources from a USD scene: an optional Scene input and a chosen camera place a flare for every matching UsdLux light, with its position projected through the camera, its angle from the camera's field of view, and its intensity and colour from the light. An optional Depth input dims lights hidden behind geometry: the depth image is sampled over each light's projected footprint and compared with the light's own depth. Knob lights (M466 - LensFlare Ghost Tracing) still work alongside.

## Scope

**In:**
- Optional "Scene" and "Depth" inputs; knobs `usdLights` (on when Scene is connected), `camera` (choice of the scene's cameras, default the scene's first camera as USDRender does), `lightPattern` (prim path pattern, default all UsdLux lights), `usdIntensityScale` (1), `depthLayer` (layer choice, default `depth.Z`), `occlusionSamples` (8), `occlusionTolerance` (0.01, relative).
- Per light per frame: image position by the camera's projection (time-sampled, so animated lights and cameras follow); direction relative to the camera axis; `sensorSize` taken from the camera's apertures (overriding the knob, which is disabled while a camera drives it); intensity = UsdLux intensity × 2^exposure × colour (× colour temperature when enabled) × `usdIntensityScale`.
- Occlusion: for sphere, disk, rect and cylinder lights, `occlusionSamples` points over the light's shape projected to the image; visible fraction = samples whose depth.Z ≥ the point's camera-axis distance × (1 − tolerance); flare intensity × visible fraction. Point lights use one sample.

**Out:**
- Ray-cast occlusion against scene geometry: not chosen (user, 2026-11-16: depth pass input).
- Dome and distant lights: no position to flare from; skipped (distant lights could be added later as sun flares).
- Light linking, shadow linking and per-light flare overrides in USD attributes: none.

**Edge cases:**
- Light behind the camera: skipped.
- Light outside the frame: flared from its projected position, as knob lights are; occlusion samples outside the Depth image's RoD count as visible.
- Depth input unconnected: no occlusion.
- Depth with a different format from LensFlare's: sampled in canonical coordinates.
- Scene with no camera: a node error while `usdLights` is on.

**Assumptions:**
- Unconfirmed: USDRender's `depth.Z` (distance along the camera's view axis in scene units, `USDRender.h:73`) is the convention; a depth from elsewhere must match it.

## Phase 470.1: USD lights

- [ ] M470.P1.T1 — Lights from the scene through a camera
  - files: `Engine/Nodes/Generator/LensFlare.{h,cpp}`, `Engine/Nodes/Generator/LensFlareUsdLights.{h,cpp}` (new), `Tests/Scene/LensFlareUsdLights_Test.cpp` (new)
  - approach: Add the Scene input and knobs. List cameras and resolve the default as USDRender does (see `Engine/Nodes/Scene/USDRender.cpp` and `SceneProjectionCameras.h`). At render time collect UsdLux lights matching `lightPattern` at the render time, project their positions through the camera to the format, compute the direction from the camera axis, and read intensity, exposure, colour and colour temperature. Feed them to the ghost and starburst stages as extra lights; include the light and camera attributes in the content hash so edits invalidate.
  - verify: gtest on an in-memory stage: a sphere light on the camera axis lands at the format centre; an animated light moves its flare between frames; doubling exposure doubles the flare energy; a light behind the camera gives no flare; the camera drives `sensorSize`.
  - size: L

- [ ] M470.P1.T2 — Depth-pass occlusion
  - files: `Engine/Nodes/Generator/LensFlareUsdLights.{h,cpp}`, `Engine/Nodes/Generator/LensFlare.cpp`, `Tests/Scene/LensFlareUsdLights_Test.cpp`
  - approach: Add the Depth input, `depthLayer`, `occlusionSamples` and `occlusionTolerance`. Per light, distribute samples over its shape (sphere silhouette, disk, rect, cylinder) with a fixed pattern, project each, fetch depth at those points (a RoI of the points' bounding box), and multiply the light's intensity by the visible fraction.
  - verify: gtest: a USDRender depth of a wall in front of half a rect light halves its flare energy within 1/`occlusionSamples`; no wall gives full energy; unconnected Depth gives full energy.
  - size: M

- [ ] M470.P1.T3 — Docs and GUI check
  - files: `Engine/Nodes/Generator/docs/LensFlare.md`, `Tests/gui/m470_lensflare_usd_lights.py` (new)
  - approach: Document scene lights, camera, depth occlusion and the depth convention. The GUI script builds USDLight → USDCamera → USDRender (depth) → LensFlare, animates a light passing behind a card and screenshots three frames under Xvfb.
  - verify: the GUI script passes; screenshots shared with the user (collected at F31 - Lens Flares' UAT).
  - size: M

**Verification gate:** build and ctest green; the GUI script's frames show the flare fading as the light passes behind the card.

## Decisions

- 2026-11-16 — **USD lights through a camera are in scope, inside F31 - Lens Flares (user, /cat-plan grill).**
- 2026-11-16 — **Occlusion from a depth pass input (user):** chosen over ray-casting the scene or a matte input.
