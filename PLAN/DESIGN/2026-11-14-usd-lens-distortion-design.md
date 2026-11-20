# Lens distortion in the USD scene: design and recommendation (2026-11-20)

The design for M438 - Lens Distortion In The USD Scene. The evidence is in
`2026-11-14-usd-lens-distortion-research.md` ("research" below). The user chooses the encoding
before any implementation milestone is filed. That choice is the open question at the end.

## Goal

A camera's lens distortion travels in the USD scene with the camera. CardProject25D, USDRender,
USDProject3D and USDProjectUV then apply it without extra wiring. It may live anywhere that is
associated with the camera (user, 2026-11-14).

## Constraints from the research

- No standard to adopt. Hydra's `HdLensDistortionSchema` covers one model family. Stock usdImaging
  does not fill it (probe), and no bundled delegate reads it. The data is a Natron convention,
  read by Natron on the composed `UsdStage`.
- No bundled delegate distorts primary rays from scene data. USDRender distorts in 2D after the
  render. Natron Utility, our own ray tracer, can later do it exactly.
- Hydra never follows relationships. Natron resolves the camera-to-lens association and passes
  sampled values to anything shader-side, as USDProject3D already does for its camera.
- Any `asset` attribute round-trips through USDWrite, `natron://` live images included
  (`UsdUtilsModifyAssetPaths`, `USDWrite.cpp:1786-1813`).

## Where it lives

| | A. Attributes on the camera (multiple-apply API, `NatronLensAPI:<name>`) | **B. Lens prim, related from the camera** | C. UsdShade lens material bound to the camera |
|---|---|---|---|
| Travels with the camera | Always | Yes when the lens is a child of the camera (the default); a target outside it can be pruned away | Only if the material travels too |
| Several lens setups | Instance names | One prim each, ordered targets | One material each |
| Shared by cameras (stereo pair, witness cams) | Duplicated | One prim, several cameras target it | Yes |
| Maps as `asset` attributes | Yes | Yes | Yes, as texture-shader inputs |
| Delegate side effects | None | None (an unknown typed prim makes no Hydra prim) | Delegates see a material with no terminal they know. Probably ignored, unverified |
| Prior art | Hydra's flat `lensDistortion:*` attributes | Karma's lens material is a separate prim reached by a relationship | Karma, literally |
| Clutter | Dozens of `natron:lens:<name>:…` attributes on the camera | Contained in its own prim | Shader graph for flat data |

**Recommended: B.** It follows the closest prior art (a separate prim reached from the camera), it
keeps the camera schema clean, and it is the only option that shares one lens between cameras
without copying. The relationship is invisible to Hydra, but that costs nothing because Natron
resolves every lookup itself.

## Encodings compared

| Criterion | Parametric (`LensModels.h` model and parameters) | Maps (STMap/IDistort images) |
|---|---|---|
| Exactness | Exact: closed form one way, Newton to 1e-12 the other way (`LensModels.h`) | Limited by map resolution and bilinear filtering; the other direction needs a numerical inversion at map resolution (M433 - Overlay Point Maps' inverter) |
| Lens coverage | The eight models LensDistortion has | Any lens, including a vendor STMap, a measured grid, or another tool's export |
| Animation | Per-parameter time samples, linearly interpolated between samples, so sub-frame motion blur samples are smooth | Time-sampled `asset` values (one image per frame), held between samples with no interpolation |
| Resolution independence | Yes. Only pixel-unit parameters (PanoTools d, e, g, t) need the reference size | No. Overscan has to be baked into the data window, and IDistort values are pixels of the reference size |
| Delegate support | None (research) | None directly. A map is exactly what a shader can sample (USDProject3D) |
| Authoring from LensDistortion | Copy its knobs as they are: same names, same table | Render its STMap output, then reference it as a live image or export it to a file |
| USDWrite and USDRead round trip | Plain float, token and int attributes; trivially intact | Asset paths: `natron://` becomes EXRs with Export Live Textures, and files are rewritten relative or absolute |
| Size on disk | A few hundred bytes per sample | One float EXR per frame for an animated lens |
| Cost in consumers | Evaluate the model: CPU and GPU code exist (`LensModelsGpu.h`, `lensdistortion.slang`) | Load and render an image (a graph render for `natron://`, a file read otherwise), asynchronously, and cancel quickly |

## Recommendation

**Both, in one lens prim type, with parametric as the primary form.**
- A lens holds a parametric model, maps, or both. When both are present, consumers use the model,
  and the maps are informational (for example a baked preview).
- Parametric covers every lens a solve in our eight models describes. It is exact, it
  interpolates, it is tiny, and it needs no image I/O in consumers.
- Maps cover lenses that have no model.
- Shader-side consumers never need an authored map. USDProject3D bakes a map from the model
  itself, internally (see Consumers).

If the user wants to stage the work, parametric goes first, because every consumer is simpler
with it. Maps are one later milestone (F below).

## Schema sketch

A codeless typed schema, registered through `plugInfo.json` and a `generatedSchema.usda` next to
the existing `Engine/Nodes/Scene/plugInfo.json`. No C++ schema classes are generated. Names are
illustrative; the type and namespace prefixes follow whatever M385 - OpenNucleon Code Rename
settles.

```usda
def Camera "shotCam"
{
    rel natron:lenses = [</shot/shotCam/plate>, </shot/shotCam/shot>]

    def NatronLens "plate"
    {
        # Parametric form. Omitted (or "none") when the lens is map-only.
        uniform token model = "3deradial4"   # LensDistortion's Model ids
        float parameters:tde4_Distortion_Degree_2.timeSamples = { 1001: -0.021, 1080: -0.034 }
        float parameters:tde4_U_Degree_2 = 0.0012
        # ...one attribute per model knob, named by its LensDistortion script name...
        uniform bool useCameraIntrinsics = true

        # The format the lens was solved on; it spans the camera's full aperture.
        uniform int2 referenceSize = (4448, 3096)
        uniform float pixelAspectRatio = 1

        # Map form. Either or both maps may be authored.
        uniform token mapType = "stmap"      # stmap | idistort
        asset undistortMap.timeSamples = { 1001: @natron://Lens1/rgba?v=9f3c@, 1002: @...@ }
        asset redistortMap
        uniform float idistortScale = 1
    }

    def NatronLens "shot" { ... }
}
```

- **Parameters** use LensDistortion's script names under `parameters:`, so all of its model
  controls, with their defaults, carry over one to one (`kParamDefs`, `LensDistortion.cpp:93`).
  The `pfFile` knob is not stored: its keys are already the C3, C5, centre and squeeze samples.
  Unauthored parameters take LensDistortion's defaults. Two-dimensional knobs (`center`, `pfP`)
  are `float2`.
- **`useCameraIntrinsics`** (3DE models): when on, the 3DE focal length, filmback,
  lens-centre offset and focus distance come from the camera's `focalLength`, apertures, aperture
  offsets and `focusDistance` at the same time. These are converted from tenths of a scene unit to
  centimetres through the stage's metersPerUnit. A zoom or focus pull on the camera then drives
  the distortion. When off, the stored `parameters:tde4_*` values are used.
- **Frame**: the lens is defined over the camera's full aperture rectangle (`horizontalAperture`
  by `verticalAperture`, shifted by the offsets). `referenceSize` gives that rectangle in pixels
  for `LensModels::Frame` and for pixel-unit parameters. A consumer with its own format conforms
  the aperture to it by its Fit Mode, as it does today, and applies the lens in aperture space. If
  `referenceSize` × `pixelAspectRatio` disagrees with the aperture's aspect ratio by more than
  0.1%, the consumer warns and fits horizontally.
- **Map conventions** follow LensDistortion's STMap output and the native STMap and IDistort
  nodes:
  - `undistortMap` lives in undistorted space, and its red and green channels hold the normalised
    distorted-plate position each pixel reads. Applied by STMap to a plate, it undistorts the
    plate. It is also what a projection needs.
  - `redistortMap` lives in distorted space and holds undistorted positions. It is what a render
    needs.
  - (0, 0) is the bottom left of the reference format, and alpha 0 means no solution.
  - The display window is the reference format, and overscan lives in the data window (kept since
    M233.P5).
  - IDistort maps hold displacements in reference pixels, times `idistortScale`.
  - With one map authored, the other direction is its numerical inverse at map resolution, as in
    M437.

## Lookup rule

A node asks for the lens of camera `C` at time `t`, with its own **Lens** choice:
1. If `C` has `natron:lenses` authored, the candidates are its targets that are `NatronLens`
   prims, in target order. Otherwise the candidates are `C`'s `NatronLens` children, in namespace
   order.
2. The node's **Lens** dropdown sits beside each camera dropdown and offers Default (the first
   candidate), None, and each candidate by name. It is filled when the camera menu is, on the main
   thread, queued.
3. The chosen lens is read on the composed stage at `t`: the frame-held time when the node holds
   a frame, and the shutter sample times for motion blur.
4. Failures:
   - A named lens that is gone, or a dangling target, gives a node warning and no distortion.
   - An invalid `model` token, a missing map, or `Model::valid == false` gives a node error.
   - A pixel where Newton fails or a map has alpha 0 is outside, as in LensDistortion.

Several lens setups (shot lens and plate lens) are several candidates. Each consumer chooses per
camera, so CardProject25D can undistort with the Projection camera's plate lens and redistort with
the Render camera's shot lens.

## Animation

- **Parametric**: every parameter may be time-sampled. `model`, `referenceSize`,
  `pixelAspectRatio` and `mapType` are uniform. A model change mid-shot is a second lens.
  Focus breathing and zoom come either from sampled coefficients (a per-frame solve) or, for 3DE
  models, from `useCameraIntrinsics` following the camera's animated focal length and focus
  distance.
- **Maps**: `undistortMap` and `redistortMap` are time-sampled assets, one value per frame. A
  static map has one default value, so it is loaded once. Between samples the value is held, so
  motion-blur sub-frames use the frame's map.

## Consumers

- **CardProject25D** (M437 - CardProject25D): connected Undistort/Redistort inputs still win (Out
  of scope here, unchanged). Without them, the Projection camera's lens undistorts and the Render
  camera's lens redistorts, each from its own Lens dropdown.
  - The parametric form runs analytically on the CPU and GPU through the existing kernels.
  - A map-only lens loads its maps as M437 loads inputs.
  - `getTransform` concatenation is off while a lens is active, as it is with maps.
- **USDProjectUV**: per vertex, the camera's undistorted aperture position goes through the lens
  into a distorted plate UV, exactly. UVs interpolate linearly across faces, so curvature between
  vertices needs dense meshes. The tooltip says so. A map-only lens is sampled at each vertex.
- **USDRender**: it renders undistorted, with the aperture enlarged to the undistorted bounds of
  the output format. Those bounds come from mapping the format's border through the lens, sampled
  per pixel along the edges. A non-finite bound is an error, and the size is never capped. The
  render is then warped into the distorted frame with the redistort direction.
  - Colour AOVs use the chosen filter.
  - `depth`, `primId` and `instanceId` use nearest sampling.
  - Natron Utility may later skip the warp by distorting its primary rays (milestone G).
- **USDProject3D**: Natron bakes an undistort map for the projector's lens (from the model, or
  taken from the lens's own map) at the plate's resolution plus the overscan it needs.
  - It serves the map as a `natron://` live image and authors it on a new `lensMap` input of the
    projection shader.
  - Each network (Storm glslfx, Cycles, MoonRay) gains one texture lookup: projected st, then
    `lensMap`, then plate st.
  - The lens is sampled at the hold frame when Use Frame Hold is on.
  - When the camera is resolved on the rendering stage (`natron:projectionCamera`), the lens is
    resolved there too.
  - USDWrite's projection bake (`authorResolvedProjector`) carries `lensMap` like the plate.

## Authoring

- **USDCamera** gets a Lens Distortion group: Lens Name, Model, and every model control from
  `kParamDefs` with LensDistortion's defaults and labels, plus Use Camera Intrinsics, Reference
  Size and Pixel Aspect Ratio. The group is built from the same table, so the two nodes cannot
  drift. Off by default.
- **USDLensDistortion** (new scene node, in-out): adds or edits a named lens on the cameras a prim
  pattern selects, for cameras from USDRead files.
  - It has the same controls, and an Add To Lenses option (append to `natron:lenses` or replace
    it).
  - It has optional Undistort Map and Redistort Map image inputs, which author `natron://` live
    images when maps are in scope.
- **LensDistortion** gains an optional Scene input with Camera and Lens dropdowns, and a Model
  Source choice (Knobs / Scene). With Scene, the model and parameters come from the lens at each
  frame, and the knobs show them read-only. Plate undistortion then uses the same lens the 3D
  nodes use.

Getting a solve from LensDistortion into the scene goes through USDLensDistortion's knobs. They are
expression-linked, or copied by a Copy From LensDistortion button that copies the knobs and their
keys.

## Round trip

- **USDWrite**, in both Layered and Flattened mode, writes lens prims and `natron:lenses` like any
  other data. Maps go through the existing asset rewrite and Export Live Textures. A
  `natron:lenses` target outside the written stage gives a warning.
- **USDRead** reads our files back with the lens prims typed, because the schema is registered at
  start-up. Other tools see an unknown prim type with its data intact. Files without lens prims
  have no distortion. Hydra's `lensDistortion:*` camera attributes are not imported, because
  nothing writes them that we know of. A k1/k2 export of them for interop is possible later; it
  is not proposed.

## Rejected

- **Hydra's `lensDistortion:*` attributes on the camera** as our encoding: one model family, not
  filled by usdImaging, and read by no bundled delegate.
- **A UsdShade lens material** (C): a shader graph for flat data, a material sprim in every
  delegate, and nothing gained over B.
- **Applying a lens inside Storm** (a vertex-shader distortion): it is wrong for long edges and
  diverges from the other delegates.

## Follow-up milestones (sketch)

| ID | Name | Content | Depends on | Size |
|---|---|---|---|---|
| A | Lens Schema And Camera Authoring | Codeless `NatronLens` schema; `SceneLens` read/lookup/write helpers that build a `LensModels::Model` at a time; the USDCamera Lens Distortion group; the USDLensDistortion node (parametric); USDWrite/USDRead round-trip tests | M132 | L |
| B | LensDistortion From Scene | Scene input, Camera and Lens dropdowns, Model Source; Copy From LensDistortion on USDLensDistortion | A | M |
| C | Scene Lens In Projection Nodes | CardProject25D's scene lens source (CPU and GPU), USDProjectUV per-vertex distortion | A, M437 | M |
| D | USDRender Lens Redistort | Automatic undistorted overscan, the post-warp, nearest sampling for id/depth AOVs | A | L |
| E | USDProject3D Lens Maps | Internal undistort-map bake, the `lensMap` input in the Storm, Cycles and MoonRay networks, the USDWrite bake | A | L |
| F | Lens Maps In The Scene (maps only) | `undistortMap`/`redistortMap`/`mapType`, image inputs on USDLensDistortion, file maps, IDistort, single-map inversion, map loading in C, D and E | A, C, D, E | L |
| G | Natron Utility Distorted Rays (optional) | Primary rays through the lens in Natron Utility, so USDRender skips the warp there | D, F7 on main | M |

## Open question for the user

**Which encoding should the scene carry?**
1. **Parametric only** (A to E, G optional).
   - Pros: exact; resolution-independent; interpolates; tiny; all eight models already exist on
     CPU and GPU; no image loading in consumers.
   - Cons: a lens with no model (a vendor STMap, a measured grid, another tool's map export) cannot
     travel in the scene. It stays on CardProject25D's map inputs and the 2D STMap/IDistort nodes.
2. **Maps only** (A without parameters, F, and C to E on maps).
   - Pros: any distortion, and a trivial shader lookup.
   - Cons: limited by resolution; needs baked overscan and an inversion for the other direction;
     image I/O in every consumer; large exports; no sub-frame interpolation; authoring from
     LensDistortion is a render-then-reference step.
3. **Both, parametric first** (A to F, G optional). Recommended.
   - Pros: covers every lens. The parametric path ships first and maps follow as F.
   - Cons: one more L milestone, and a second path in each consumer.

**Sub-question: where it lives.** The recommendation is a lens prim (a child of the camera by
default) that the camera's `natron:lenses` relationship selects, as above (B). The alternative is
attributes on the camera through a multiple-apply API (A): simpler and always attached, but
duplicated across cameras and crowding the camera.
