# Lens distortion in the USD scene: prior-art survey (2026-11-20)

Input to M438 - Lens Distortion In The USD Scene. The question: what already exists for carrying a
camera's lens distortion in a USD scene, and what the bundled render delegates can consume. The
design and recommendation are in `2026-11-14-usd-lens-distortion-design.md`.

Evidence, in order of strength:
- **Probe** (executed): a small C++ program built and run in the dev image
  `ghcr.io/charlesangus/natron-dev:2c1cbed46e197e27` (OpenUSD 26.08, `HD_API_VERSION 103`). It is
  described under "Probe" below.
- **Headers** (read): `/usr/local/include/pxr` from the same image. No pxr `.cpp` sources are in
  the image, so the usdImaging behaviour rests on the probe, not on reading the code.
- **Binaries** (`strings`): the plug-ins in a current AppImage (`build/m262-main/squashfs-root`):
  `plugin/usd/hdCycles.so`, `plugin/usd/hdStorm.so`, `lib/libusd_hd*.so`,
  `moonray/lib64/libhydramoonray.so`, `moonray/rdl2dso/*Camera.so`.
- **Source** (read): the Cycles fork the image builds (`tools/ci/local/build-cycles.sh`: fork of
  Cycles v5.2.0 with the deep-output and traced-lens commits), `src/hydra/camera.cpp`; Natron
  Utility on `origin/feature/f7-natron-utility-core`.
- **Docs** (read): `.local/docs` trees for USD, Solaris, Katana 9.0, Gaffer 1.7.5, Blender 5.3.

## Summary

| Source | Lens distortion in the scene? | Form | Reference |
|---|---|---|---|
| OpenUSD scene schemas (`UsdGeom`, `UsdRender`, all `generatedSchema.usda`) | No | — | `lib/usd/*/resources/generatedSchema.usda`: the only "lens" text is `fStop`'s "Lens aperture." |
| Hydra (`hd`) | Yes, Hydra-side only | Parametric: `type` (`standard`, `fisheye`), `k1`, `k2`, `center`, `anaSq`, `asym`, `scale`, `ior` | `pxr/imaging/hd/lensDistortionSchema.h`, `camera.h:69-79`, `version.h:55` |
| usdImaging (USD to Hydra) | **No**: authored `lensDistortion:*` attributes do not reach Hydra | — | Probe |
| Storm | No | — | `hdSt`/`hdx` binaries: no `lensDistortion` string |
| hdCycles (our fork) | No via Hydra; the Cycles core has lens features | Fisheye polynomial and traced lens prescription exist in Cycles, but `hydra/camera.cpp` reads only the standard camera schema | `src/hydra/camera.cpp:54-170`; `hdCycles.so` strings |
| MoonRay (hdMoonray) | No | Hydra makes only `PerspectiveCamera`; the rdl2 cameras have no distortion attributes | `libhydramoonray.so`, `rdl2dso/PerspectiveCamera.so` strings |
| Natron Utility | No (yet) | Plain `HdCamera` and its projection matrix | `UtilityRenderDelegate.cpp:614`, `UtilityRenderPass.cpp:579` on F7 |
| Solaris / Karma | Yes, renderer-specific | A **lens material** bound to the camera by a relationship; parametric curvature or a texture map | Karma User Guide, Cameras; Camera LOP |
| Copernicus / VOPs | 2D and shader nodes, not scene data | Parametric (OpenCV k1-k6, p1, p2) | Lens Distort COP, Lens OpenCV Distort VOP |
| Katana 9.0 | Nothing in the docs | — | `rg -i distort .local/docs/katana`: 0 files |
| Gaffer 1.7.5 | Renderer-specific | Map: Arnold camera `uvRemap`, a shader giving distorted UVs | GafferArnold ArnoldCameraShaders |
| Blender 5.3 | Per movie clip, not per camera; not in USD I/O | Parametric (Polynomial, Division, Brown and a k1/k2 model) | Movie Clip tracking camera; USD import/export |

No tool or schema writes lens distortion into USD in a form another tool reads. The nearest
standard is Hydra's `HdLensDistortionSchema`, and nothing we bundle feeds it or reads it.

## OpenUSD

**Scene schemas.** `UsdGeomCamera` (`usdGeom/resources/generatedSchema.usda`, `class Camera`) has
projection, apertures and their offsets, focal length, clipping, `fStop`, `focusDistance`, stereo
role, shutter and exposure. It has nothing for distortion. A case-insensitive search for
`distort|lens` over every bundled `generatedSchema.usda` finds only the `fStop` brief "Lens
aperture." `UsdRender` (RenderSettings, RenderProduct) has no lens attributes either. The
`.local/docs/usd` tree has no proposal or white paper on lens distortion. Its 19 hits are all
Doxygen pages for the Hydra schema below (for example
`usd/api/class_hd_lens_distortion_schema_1_1_builder.html`).

**Hydra.** `HdLensDistortionSchema` (`pxr/imaging/hd/lensDistortionSchema.h`, Pixar 2023) is a
container under `HdCameraSchema::GetLensDistortion()` (`cameraSchema.h:62,166`). Its fields are
`type`, `k1`, `k2`, `center`, `anaSq`, `asym`, `scale` and `ior`. The legacy `HdCamera` gets the
same values through the `HdCameraTokens` `lensDistortion:type` … `lensDistortion:ior`, with the type
values `standard` and `fisheye` (`camera.h:69-79`, getters `camera.h:264-293`). It arrived with hd
API 52: "Added lens distortion, focus, and split diopter parameters to HdCamera"
(`version.h:55`). The headers do not document what the coefficients mean. The fields match the
k1/k2 radial model with an anamorphic squeeze and asymmetry, which is LensDistortion's first Model
option (its parameter struct in `LensModels.h`: k1, k2, centre, anamorphic squeeze, asymmetric x/y). `ior`
belongs to the fisheye form. This is a renderer-facing parameter set for one model family. It
cannot hold the 3DE4 models, PFBarrel or PanoTools.

**usdImaging does not populate it.** In the probe, a `UsdGeomCamera` with authored
`lensDistortion:type`, an animated `lensDistortion:k1` and `lensDistortion:center` went through
`UsdImagingStageSceneIndex`. The camera container held only the schema attributes (projection,
apertures, offsets, focal length, clipping range and planes, fStop, focusDistance, stereoRole,
shutter, exposure). `HdCameraSchema::GetLensDistortion()` was undefined, and so was a custom
`natron:lens:model` attribute. So a delegate that wanted the parameters (hdPrman reads them, but it
is not bundled) could not get them from a plain USD file through stock usdImaging 26.08. That
would take a scene-index plug-in of our own.

**Relationships in Hydra.** Natron already relies on the fact that Hydra reads only sampled values
and never a relationship's target (`Engine/Nodes/Scene/SceneProjectionCameras.h:32-37`). USDProject3D
therefore names its camera by a path attribute that the rendering stage resolves. Any
relationship-based lens association has to be resolved on the composed `UsdStage` by Natron, never
inside a delegate.

## Bundled delegates

- **Storm**: `libusd_hdSt.so` and `libusd_hdx.so` contain no `lensDistortion` string. Storm
  rasterises with a projection matrix, so distortion would have to be a post-process.
- **hdCycles** (fork at `natron/deep-5.2`, built by `tools/ci/local/build-cycles.sh`):
  `src/hydra/camera.cpp` reads projection, apertures and offsets, focal length, clipping, fStop,
  focus distance, shutter and the transform (`camera.cpp:36-170`). It reads no lens distortion,
  and `hdCycles.so` has no `lensDistortion` string. The Cycles core can do more, but nothing in
  the Hydra layer selects it:
  - panorama types, including `fisheye_lens_polynomial`, a 4th-degree polynomial from sensor
    position to angle (`.local/docs/blender/manual-5.3/render/cycles/object_settings/cameras.md:79-94`);
  - the fork's traced lens prescription (commit 86bf4b0, "Camera: real lens (traced prescription)
    for the perspective camera": sockets `lens_surfaces`, `lens_focal_length`, `lens_fstop`, …),
    which produces physical distortion and focus breathing.

  Neither is a model fitted to a plate. The polynomial is a fisheye mapping, and the prescription
  is a lens design, not a 3DE/PFBarrel solve.
- **MoonRay**: `libhydramoonray.so` creates only `PerspectiveCamera` (the string list has no other
  camera class), and has no `lensDistortion` string. The rdl2 camera DSOs (`Perspective`,
  `Orthographic`, `Fisheye`, `Spherical`, `DomeMaster3D`, `Bake`) have no distortion attributes in
  their string tables. `ProjectCameraMap` projects textures but has no distortion either.
- **Natron Utility** (F7, not yet on `main`): `UtilityRenderDelegate.cpp:614` creates a plain
  `HdCamera`, and the render pass takes `renderPassState->GetProjectionMatrix()`
  (`UtilityRenderPass.cpp:579`). It has no distortion today. Because it is our own ray tracer,
  it is the one delegate where distorted primary rays are cheap to add. M288 - Utility Renderer
  Projection Modes lists "Fisheye and lens-distortion projections" as out of scope.

## Solaris and Karma

From the `.local/docs/solaris` tree, Karma User Guide > Cameras (`…/solaris/kug/cameras.html`,
markdown copy `…/solaris/kug/cameras.md:20-28, 55-102`), and the Camera LOP reference
(`…/nodes/lop/camera.md:297-304`):
- "USD cameras don't have the ability to describe sophisticated physical camera effects entirely
  on their own. For this we rely on Karma's support for Lens Materials."
- The camera has `karma:camera:use_lensshader` and a **Lens Material**, a path to a USD material
  bound to the camera. The docs say "a camera lens material is treated by USD just like materials
  assigned to geometry". It is a separate prim reached from the camera by a binding relationship.
- That material's Lens Distortion section offers a curvature type of None, Quadratic, Cubic or
  **Texture Map**. With Texture Map, an image holds the lens "height" per position
  (`…/nodes/vop/lens_distort.md:12-15`), along with chromatic aberration.
- Lens materials "can be animated", which plain material inputs allow through time samples.
- Separately, the Lens OpenCV Distort VOP (`…/nodes/vop/lens_opencv_distort.md`) and the Lens
  Distort COP (`…/nodes/cop/lensdistort.md`) take OpenCV k1-k6, p1 and p2. They are 2D or shader
  parameters, not scene data on the camera.

The idiom: distortion is not on the camera prim. It is a separate shading prim that the camera
binds, and it can be parametric or a map. It is renderer-specific (`karma:` namespace), so no
other tool reads it.

## Katana

`rg -i distort .local/docs/katana` finds no file. The 9.0 docs describe no scene-level lens
distortion. Distortion there is a renderer camera-shader matter, outside these docs.

## Gaffer

`GafferScene/Camera.html` lists projection, aperture, focal length, offsets, fStop, focus,
clipping and render overrides, with no distortion. The only lens-distortion feature is
renderer-specific. Arnold's camera `uvRemap`
(`gaffer/1.7.5.0/Reference/NodeReference/GafferArnold/ArnoldCameraShaders.html`) is "a shader used
to simulate lens distortion effects. The shader is evaluated across a 0-1 UV range that is mapped
to the camera's screen space, and should output a red/green UV image of distorted UV positions."
That is a map encoding: an STMap in screen space, connected to the camera as a shader.

## Blender

- Distortion lives on the movie clip's tracking camera, not on the scene camera
  (`manual-5.3/movie_clip/tracking/clip/sidebar/track/camera.md:30-58`). The models are Polynomial
  (K1-K3), Division, Brown (K1-K4, P1, P2) and a k1/k2 model. The Movie Distortion compositor node
  applies them in 2D (`compositing/types/transform/movie_distortion.md`).
- USD import and export (`files/import_export/usd.md:144-160, 438-440`) cover projection, focal
  length, focus distance, f-stop, clipping, sensor shift and size, and nothing for distortion.
- Cycles' fisheye polynomial is above, under hdCycles.

## What Natron has today

- **Models**: `Engine/Nodes/Transform/LensModels.h` defines eight model types: the k1/k2 radial
  model, PFBarrel, 3DE Classic, 3DE4 Anamorphic Degree 6, Radial Fisheye Degree 8, Radial Standard
  Degree 4, Anamorphic Standard Degree 4, and PanoTools. Each runs in both directions (closed form
  one way, a damped Newton solve the other way), with analytic Jacobians and GPU mirrors
  (`LensModelsGpu.h`, `Engine/Gpu/Kernels/lensdistortion.slang`). Points are pixel coordinates of a
  reference format, with a pixel aspect ratio (`LensModels::Frame`).
- **Node**: `LensDistortion` (M132 - Native Lens Distortion) has one knob per model parameter,
  defined in a table (`LensDistortion.cpp:81-158`, `kParamDefs`). Script names are in
  `LensDistortion.h`, for example `k1`, `anamorphicSqueeze`, `pfC3`, `tde4_Distortion_Degree_2`
  and `tde4_focal_length_cm`. Model is a non-animated choice (`LensDistortion.cpp:335`). The node
  can output an STMap. Its convention: red and green hold the normalised position in the Source
  format that each pixel reads, and alpha is 0 where the model has no solution.
- **Cameras in nodes**: scene inputs with a camera-path dropdown (`HydraRender::cameraPaths`,
  `HydraRenderImpl.h:105`), conformed to a format by `HydraRender::FitModeEnum`
  (`HydraRenderImpl.h:80`). USDProject3D resolves a camera that is off its input by path on the
  rendering stage (`USDProject3D.h:75-84`).
- **Live images in the scene**: `natron://<node>/<layer>?v=<hash>` URIs served by an in-process
  `ArResolver` and `HioImage` (`LiveTextureStore.h:49`, `Engine/Nodes/Scene/plugInfo.json`).
  USDWrite rewrites **every** asset-valued attribute in the layers it writes through
  `UsdUtilsModifyAssetPaths` (`USDWrite.cpp:1786-1813`). With Export Live Textures, `natron://`
  values become per-frame EXRs, so a map held in any `asset` attribute round-trips, provided its
  source is in the payload's live-texture list.
- **No schema plug-in yet**: Natron registers no USD schema types. Its `plugInfo.json` holds only
  the resolver and the image reader.

## Probe

`/tmp/m438-probe/probe.cpp`, built with gcc 14 against `/usr/local/lib` in the image and run once.
In outline, it does the following:

```
UsdGeomCamera::Define(stage, /cam)
  lensDistortion:type   token  = standard
  lensDistortion:k1     float  = {1: 0.1, 11: 0.3}
  lensDistortion:center float2 = (0.01, 0.02)
  natron:lens:model     token  = x
UsdImagingStageSceneIndex::New(); SetStage; SetTime(6); GetPrim(/cam)
```

Output: `type camera`; camera container names: `projection horizontalAperture verticalAperture
horizontalApertureOffset verticalApertureOffset focalLength clippingRange clippingPlanes fStop
focusDistance stereoRole shutter:open shutter:close exposure exposure:iso exposure:time
exposure:fStop exposure:responsivity`; `lensDistortion defined 0`.

## Conclusions for the design

1. No standard exists to adopt. Hydra's schema covers one model family, stock usdImaging does not
   fill it, and no bundled delegate reads it. Whatever we author is a Natron convention, consumed
   by Natron on the composed stage.
2. No bundled delegate can distort primary rays from scene data. USDRender has to distort in 2D
   after rendering, or teach Natron Utility (our own delegate) distorted ray generation.
3. The prior art uses both encodings. Karma's lens material is parametric or a texture map. The
   Arnold `uvRemap` Gaffer exposes is an STMap. All the 2D tools are parametric. The closest
   idiom is Karma's: a separate prim that the camera reaches by a relationship.
4. Relationships are fine for Natron-side lookups but invisible to Hydra. Shader-side consumers
   (USDProject3D's networks) need the lens resolved by Natron into sampled inputs, as the projector
   camera already is.
5. Maps already round-trip: any `asset` attribute holding a `natron://` URI or a file path goes
   through USDWrite's export unchanged in shape.
