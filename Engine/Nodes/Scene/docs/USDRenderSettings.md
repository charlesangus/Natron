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

## Renderer tabs

Every render delegate the build has gets a tab, labelled with the delegate's
name (Storm, Cycles, MoonRay, ...). Its knobs sit in groups, listed below. Each
knob's tooltip says what it does, what it trades off and which values to try,
and says so when the control has no effect in this build.

### Cycles

- Sampling: samples, adaptive sampling, time limit and seed.
- Denoise: whether and how the image is denoised.
- Light Paths: how far light travels through the scene.
  - Bounces: the bounce limits, in total and for each kind of bounce.
  - Clamping: the brightness limits on samples, against fireflies.
  - Caustics: reflective and refractive caustics, and Filter Glossy.
  - Fast GI: the approximation of distant indirect light.
- Lights: Use Light Tree and Light Sampling Threshold.
- Volumes: ray marching and its steps.
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
`moonray:sceneVariable:pixel_samples`. The Viewer's Max Samples
(`maxSamples`), when it is not 0, applies last and overrides the prim's
samples. The view's refinement restarts only when an edit touches the
settings of the view's own renderer or something outside `/Render`. An edit
on another renderer's tab leaves the refinement alone.

## Render caching

A USDRender's cached frames depend only on the settings of the renderer it
renders with. Editing a knob on the MoonRay tab while USDRender renders with
Cycles keeps the cached Cycles frames. Switching the renderer, or editing a
knob on the active renderer's tab, renders again. A disabled USDRenderSettings
authors nothing and adds nothing to the render's hash.

## RenderSettings prims from files

A scene read with USDRead keeps the RenderSettings prims it has under `/Render`
and its `renderSettingsPrimPath`.

- USDRender's Settings Prim menu lists those prims next to Stage Default, which
  renders with the file's active prim.
- The renderer settings in a prim's attributes (`cycles:samples`, ...) apply
  the same way as the ones this node authors.
- `natron:renderer` is Natron's own attribute. A prim from another tool usually
  lacks it, so Scene Default renders such a prim with Storm unless the
  USDRender or the view chooses another renderer.

To change a prim from a file, set a USDRenderSettings node's Prim Path to the
prim's path. The node's values are stronger than the file's. To keep the
file's prim active, turn Make Active off.
