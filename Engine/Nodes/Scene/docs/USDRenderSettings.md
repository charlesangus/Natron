# USDRenderSettings

Authors a RenderSettings prim (UsdRenderSettings) over its input scene. The prim
says how the scene renders: the camera, the resolution, how the camera's
aperture fits that resolution, which renderer to use and that renderer's
settings. USDRender renders with it, and so does the scene view in the
Viewer when its renderer is Scene Default. Because the settings are prims of
the scene, USDWrite writes them out with it and other USD tools can read them.

## Controls tab

- Prim Path (`primPath`): the path of the prim the node authors,
  `/Render/rendersettings` by default. RenderSettings prims belong under
  `/Render`, and the menus that list them only look there.
- Camera (`camera`): one of the input scene's cameras, authored as the prim's
  `camera` relationship. If the scene loses a chosen camera, the camera stays
  chosen and a USDRender using the prim reports an error that names its path.
- Format (`NatronParamFormatChoice`, `NatronParamFormatSize`,
  `NatronParamFormatPar`): the resolution and pixel aspect ratio, authored as
  `resolution` and `pixelAspectRatio`. The project's format is the default.
- Conform Policy (`conformPolicy`): how the camera's aperture is fitted to the
  format when their aspect ratios differ (`aspectRatioConformPolicy`).
  - Expand Aperture (the default): grows the aperture so that everything the
    camera saw stays in view.
  - Crop Aperture: shrinks it and crops what does not fit.
  - Adjust Aperture Width: keeps the height and changes the width.
  - Adjust Aperture Height: keeps the width and changes the height.
  - Adjust Pixel Aspect Ratio: keeps the aperture and changes the pixel aspect
    ratio. USDRender does not do this one. It renders with Expand Aperture
    instead and shows a warning.
- Data Window (`dataWindow`): the part of the format to render, in normalized
  coordinates (`dataWindowNDC`).
- Disable Motion Blur (`disableMotionBlur`) and Disable Depth Of Field
  (`disableDepthOfField`): authored for renderers and tools that read them.
  USDRender always renders the whole format and does not read these three
  attributes (Data Window and the two Disable switches).
- Transparent Background (`transparentBackground`, off by default): hides the
  dome light and the world background from the camera, so the background
  renders with zero alpha while the dome still lights the scene. It authors
  `cycles:background:transparent` for Cycles and
  `storm:domeLightCameraVisibility` for Storm, and wins over the Transparent
  setting on the Cycles tab. Off, it authors neither, and each renderer keeps
  its own default.
- Default Renderer (`defaultRenderer`): the renderer for the prim, authored as
  `natron:renderer`. Storm is the default. If the chosen renderer is not
  installed or cannot run here, it stays chosen and is marked unavailable.
- Make Active (`makeActive`, on by default): makes this prim the scene's
  render settings by authoring the stage's `renderSettingsPrimPath`. Turned
  off, the node still authors the prim, but whichever prim the input scene
  already made active stays active.

## AOVs tab

The AOVs tab picks the AOVs a render makes besides the colour, depth and ID
layers it always has. Each pick is authored as a RenderVar in a scope under
`/Render/Products/Vars` named after the prim, named after its source
(`cycles:normal` is `/Render/Products/Vars/rendersettings/cycles_normal` by
default), and listed in the `orderedVars` of a RenderProduct under
`/Render/Products` named after the prim (`/Render/Products/rendersettings` by
default). The prim lists the product in its `products` after any products the
input scene gave it, and a render makes the vars of all of them. With nothing
picked, the node authors no product. A var of the same path in the input scene
takes this node's source, data type and layer.

- One group per installed renderer that Natron knows the AOVs of (Cycles AOVs,
  MoonRay AOVs, Storm AOVs, ...), with a switch per AOV. The list is the
  renderer's own when its delegate publishes one, else the AOVs Natron knows it
  renders. A switch's script name is the renderer's prefix, `Aov` and the AOV's
  name without the renderer's namespace (`cyclesAovAo` for `cycles:ao`,
  `cyclesAovDiffuseDirect` for `cycles:diffuse_direct`, `moonrayAovP`).
- A ticked AOV is authored in the renderer's namespace (`cycles:normal`,
  `moonray:P`), so other renderers leave it out: ticks for several renderers
  can sit on one node, and each render takes only its own.
- Custom AOVs (`customAovs`): a table of RenderVars the groups do not list, one
  per row. Name is the `sourceName`: a primvar, a light path expression, or an
  AOV in a renderer's namespace (`moonray:albedo`), which only that renderer
  renders. Source Type is `raw`, `primvar` or `lpe`; empty is `raw`. Data Type
  (`color3f`, `float`, ...) sets the layer's channels until the renderer has
  given its own format; empty leaves them to the renderer. Layer names the
  layer, authored as `natron:layer`; empty names it after the AOV without its
  namespace.

USDRender renders each RenderVar to a layer of its own. A RenderVar the
renderer does not render gets a warning on the USDRender, and the other layers
still render. A RenderVar of an AOV every render has (`color`, `depth`,
`primId`, and `normal` with the renderers other than Storm), named with or
without the renderer's namespace (`cycles:normal`), fills that AOV's layer: its
settings still go to the renderer, and a Layer it names is ignored with a
warning.

## Renderer tabs

Every render delegate the build has gets a tab, labelled with the delegate's
name (Storm, Cycles, MoonRay, ...). Its knobs sit in groups, listed below. Each
knob's tooltip says what it does, what it trades off and which values to try,
and says so when the control has no effect in this build. The tab of a delegate
with no settings says only that.

### Natron Utility

- Device: Auto, GPU or CPU (see USDRender).
- Sampling: Pixel Samples (3, a 3 by 3 grid a pixel), Pixel Filter, Filter
  Width and Seed.
- Ray depths: Max Cutouts, the surfaces a ray passes through before it gives
  up, and Max Ray Depth, Reflection Depth and Refraction Depth, how far rays go
  on past the first surface they meet.
- Motion blur: Override Shutter, Shutter, Shutter Offset, Shutter Custom Offset
  and Shutter Segments, and Camera Motion Blur, Transform Motion Blur and
  Deformation Motion Blur to turn each kind of motion off.
- Subdivision: Subdiv Level (0, the cage as authored) and Max Subdiv
  Triangles, the most one mesh refines to.
- Projection: Camera, Spherical, Cylindrical or UV Unwrap, and Overscan, the
  pixels the frame grows by on every side for each projection but UV Unwrap.
  - Spherical renders a 360 by 180 degree lat-long and Cylindrical a 360
    degree band of the camera's vertical view, both from the camera's position
    in its orientation. Motion blur applies to them as to Camera.
  - UV Unwrap bakes the mesh at UV Prim into the UDIM Tile of its UV Primvar,
    the format being the texture's size. Texels no triangle covers stay empty.
  - UV Bake Bounces, off by default, decides what a texel bakes. Off, it bakes
    only the surface's own lit shading: no reflections, no refractions and
    nothing behind a partly transparent surface. On, it bakes what a head-on
    view along the surface's normal sees, within the ray depths: a mirror or a
    metal bakes the environment reflected back along its normal, and a partly
    transparent surface bakes what lies behind it blended with its own
    shading.

### Cycles

- Sampling: samples, adaptive sampling, time limit and seed.
- Denoise: whether and how the image is denoised.
- Light Paths: how far light travels through the scene.
  - Bounces: the bounce limits, in total and for each kind of bounce.
  - Clamping: the brightness limits on samples, against fireflies.
  - Caustics: reflective and refractive caustics, and Filter Glossy.
  - Fast GI: the approximation of distant indirect light.
- Lights: Match Light Levels, Use Light Tree and Light Sampling Threshold.
  Match Light Levels, on by default, lights the scene as brightly as Storm
  does for the same lights and exposure. Off, dome lights are about 3.14 times
  brighter and distant lights 4 times brighter.
- Volumes: ray marching and its steps.
- Subdivision: Subdivision Surfaces, and how finely the meshes are diced.
- Path Guiding: this build of Cycles has no path guiding, so these controls
  have no effect.
- Light Contributions: switches for kinds of light, to break down or debug an
  image.
- Film: exposure, the pixel filter and its width, a transparent background
  and the approximate shadow catcher.
- Passes: mist, Cryptomatte and the alpha threshold for data passes. Natron
  reads every pass by name, so Cycles' choice of the pass it shows, and its
  view of the pixels adaptive sampling still works on, have no knobs.
- World: whether the world shader is used, which rays see the background,
  and its light group.
- Advanced: sampling pattern, minimum bounces, motion blur and pixel jitter.
  - Sample Subset: rendering part of the sample sequence. Only Sample Offset
    has an effect.
- Other Settings: every other setting Cycles declares.

### MoonRay

- Sampling: pixel, light, material and subsurface samples.
- Depth: the bounce limits, in total and for each kind of bounce.
- Filtering: the pixel filter and its width.
- Denoise: denoising and its guides. In this build MoonRay asks for a denoiser
  it lacks, so expect the image to stay noisy.
- Scene Toggles: motion blur, lighting and how geometry and normals are read.
  - Prune: removing kinds of geometry from the render.
- Subdivision: Subdivision Surfaces, and how finely the meshes are tessellated.
- Performance: Execution Mode and Max FPS.
- Remote: rendering on other machines, which stays disabled because Natron
  renders MoonRay on this machine only.
- Logging: log levels and writing the scene to an RDLA file.
- Other Settings: every other setting MoonRay declares.

### Storm and other renderers

These tabs have no hand-picked groups. Each setting goes in a group chosen from
its name, and only groups with a setting appear: Denoise, Path Guiding, Volumes,
Bounces, Clamping, Lights, Sampling, Filtering, Motion, Performance and Logging.
A setting whose name matches none of them goes in Other Settings. Storm, for
example, has Volumes, Lights and Performance.

### Script names and attributes

A knob's script name is the renderer's prefix followed by the setting's name in
camel case. Its label is that name made readable, so `cyclesMaxDiffuseBounce`
is labelled "Max Diffuse Bounce". Each knob is authored on the prim as an
attribute in the renderer's namespace, for example `cycles:samples` or
`moonray:sceneVariable:pixel_samples`. When a renderer renders, it gets the
attributes in its own namespaces through its delegate's render settings and
ignores the rest. So one prim can carry settings for every renderer, and
switching renderers keeps each renderer's settings.

## Which renderer renders

USDRender and the scene view choose their renderer in the same order:

1. The renderer chosen on the USDRender (its Renderer knob) or in the scene
   view (its context menu's Renderer submenu).
2. If that choice is Scene Default, the renderer the active prim names in
   `natron:renderer` (this node's Default Renderer).
3. Storm, if there is no active prim or the prim names no renderer.

USDRender defaults to Scene Default. The scene view defaults to Storm, and
Scene Default is the first entry of its Renderer submenu. On Scene Default,
the view follows the prim: changing Default Renderer, Make Active or the active
prim switches the renderer the view uses.

The view applies the active prim's settings for its renderer. A Cycles view
therefore refines to the prim's `cycles:samples`, and a MoonRay view to its
`moonray:sceneVariable:pixel_samples`, which is the square root of the
samples per pixel, so 8 refines to 64. The Viewer's Max Samples
(`maxSamples`), when it is not 0, applies last and overrides the prim's
samples. It counts samples per pixel on every renderer. MoonRay gets the
square root rounded up as its pixel samples, so Max Samples 16 renders 16
samples per pixel and Max Samples 8 renders 9. The sample count the view
shows stops at Max Samples, and reaches it only once MoonRay's render is
complete. The view's refinement restarts only when an edit touches the
settings of the view's own renderer or something outside `/Render`. An edit
on another renderer's tab leaves the refinement alone.

## Render caching

A USDRender's cached frames depend only on the settings of the renderer it
renders with. Editing a knob on the MoonRay tab while USDRender renders with
Cycles keeps the cached Cycles frames. Switching the renderer, or editing a
knob on the active renderer's tab, renders again. The AOV switches follow
the same rule: a MoonRay AOV ticked while USDRender renders with Cycles keeps
the cached frames, and any edit to Custom AOVs renders again. A disabled USDRenderSettings
authors nothing and adds nothing to the render's hash.

## RenderSettings prims from files

A scene read with USDRead keeps the RenderSettings prims it has under `/Render`
and its `renderSettingsPrimPath`.

- USDRender's Settings Prim menu lists those prims next to Stage Default, which
  renders with the file's active prim.
- The renderer settings in a prim's attributes (`cycles:samples`, ...) apply
  the same way as the ones this node authors.
- The RenderProducts the prim lists, and their RenderVars, are rendered as the
  ones the AOVs tab authors. A product's own camera, resolution and other
  settings win over the prim's. Products that differ in camera or resolution
  are an error.
- `natron:renderer` is Natron's own attribute. A prim from another tool usually
  lacks it, so Scene Default renders such a prim with Storm unless the
  USDRender or the view chooses another renderer.

To change a prim from a file, set a USDRenderSettings node's Prim Path to the
prim's path. The node's values are stronger than the file's. To keep the
file's prim active, turn Make Active off.
