# USDRender

Renders its scene input into an image with a Hydra render delegate, as a
RenderSettings prim of the scene describes (see USDRenderSettings). The prim
gives the camera, the resolution and pixel aspect ratio, the aspect ratio
conform policy, the renderer and that renderer's settings. USDRender reads the
prim and has no camera or format knobs of its own.

Storm, USD's OpenGL renderer, needs an OpenGL 4.5 compatibility context. In the
application it shares the viewer's OpenGL. Without a window system
(`NatronRenderer`, or a server without X) it makes its own context through EGL,
which Mesa's llvmpipe software renderer provides. If neither is available, the
node reports why instead of rendering, and image-only graphs are unaffected.

Natron Utility is Natron's own ray tracer. It shades with the scene's materials
and lights:

- Materials: UsdPreviewSurface, its inputs constant, read from primvars or from
  textures (UDIM textures included), and the surface USDProject3D authors. A
  prim with no material shades as a UsdPreviewSurface of its display colour
  and display opacity. A material it cannot read renders magenta.
- Lights: the UsdLux distant, sphere, rect, disk, cylinder and dome lights,
  with their light and shadow links, casting shadows that surfaces with an
  opacity below 1 let some light through.
- Reflection and refraction: rays go on from a surface by reflection,
  refraction or straight through a transparent surface, as far as its Max Ray
  Depth, Reflection Depth and Refraction Depth settings allow.
- Sampling: a grid of samples a pixel, weighed by a pixel filter, and motion
  blur of the camera, of transforms (instances included) and of deforming
  meshes within the shutter.
- Subdivision: subdivision-surface meshes refine to the Subdiv Level setting,
  or to a mesh's own `natron:subdivLevel`.
- Projection: the frame looks through the camera, as a spherical lat-long, as
  a cylindrical band, or bakes one mesh's surface into a UV tile (UV Unwrap),
  and Overscan grows the frame beyond the format. A bake takes only the
  surface's own lit shading unless UV Bake Bounces is on.

The settings for all of these are on the Natron Utility tab of
USDRenderSettings. `depth.Z` and `primId.id` match Storm's.

Its Device setting picks Auto, GPU or CPU: Auto uses the GPU when it supports
ray queries and otherwise renders on the CPU, and the node says which device
rendered and why a render fell back to the CPU. A frame rendered on the GPU
keeps its `rgba` there, so GPU nodes downstream read it without a copy through
host memory.

Its Render Mode setting picks Ray or Raster. Ray traces a ray through every
sample of every pixel. Raster draws the frame Raster Samples times on the GPU,
each pass at one sub-pixel offset and one moment of the shutter for the whole
frame, finds the surface each pixel sees from that drawing and traces only
what follows from it: shading, shadows, reflections, refractions and the
surfaces seen through cutouts and transparency, as Ray does. It is faster on
simple scenes, most of all projections with motion blur. The image need not
match Ray's exactly:

- Motion blur and anti-aliasing come from the passes, so a low Raster Samples
  steps visibly, a moving object showing as that many copies. With the
  shutter closed every pass is at the frame's own time.
- A moving or zooming camera is taken at each pass's time; lights stay at the
  middle of the shutter.
- Depth, `primId` and `instanceId` take the pixel's heaviest sample, as in
  Ray; they are never averaged.
- Large frames are drawn in tiles the GPU can hold.

Raster needs the GPU and the Camera projection. A frame that cannot use it
renders as Ray and the node warns "Render Mode is Raster, but the frame was
rendered in Ray mode" with the reason: Device is CPU, the GPU cannot draw the
visibility buffer Raster needs, or the projection is Spherical, Cylindrical or
UV Unwrap. On Auto, a scene that does not fit on the GPU falls back to the CPU
in Ray mode, with the same warning.

Cancelling a Natron Utility render stops it within a fraction of a second, on
either device: between tiles and within a tile between batches of lights, while
the CPU builds the scene's acceleration structures, while the GPU updates its
own, and while meshes are refined. A stopped frame is never shown, and the next
render starts it again. An edit to the scene is taken in whole before the
frame that shows it starts.

The other delegates (Cycles, MoonRay) render on the CPU on their own threads.

## Controls

- Settings Prim (`settingsPrim`): the RenderSettings prim to render with.
  - Stage Default, the default, uses the prim the stage's
    `renderSettingsPrimPath` names: the last USDRenderSettings upstream with
    Make Active on, or the active prim of a file read with USDRead.
  - Every other entry is a RenderSettings prim under `/Render` in the input
    scene, including prims from files.
  - If the scene loses a chosen prim, the prim stays chosen and the node shows
    an error that names it.
- Renderer (`renderer`): Scene Default, then every render delegate the build
  has. Scene Default, the default, uses the renderer the prim names in
  `natron:renderer`. A renderer that is not installed is marked unavailable,
  and rendering with it is an error that says why.

The renderer is chosen in this order:

1. The Renderer knob, unless it is on Scene Default.
2. The prim's `natron:renderer`.
3. Storm.

The prim's attributes in the chosen renderer's namespaces (`cycles:samples`,
`moonray:sceneVariable:pixel_samples`, ...) are passed to the renderer as its
settings. Attributes in other renderers' namespaces are ignored.

The prim's `resolution` and `pixelAspectRatio` set the output format, and its
`camera` relationship sets the camera.

- If the prim names no camera, the scene's first camera is used.
- If the prim names a camera the scene does not have, the node shows an error
  that names the camera.

`aspectRatioConformPolicy` fits the camera's aperture to the format:

- `expandAperture` and `cropAperture` are applied as named.
- `adjustApertureWidth` keeps the aperture's height.
- `adjustApertureHeight` keeps its width.
- `adjustPixelAspectRatio` is not supported. The node renders with
  `expandAperture` and shows a warning.

If the scene has no active prim and Settings Prim is Stage Default, the scene's
first camera renders the project's format, and the node shows a warning that
says so. The renderer is then the Renderer knob's choice, or Storm on Scene
Default, with that renderer's own default settings.

The node's hash covers only the settings of the renderer it renders with, from
the prim it renders with. Editing another renderer's tab on a
USDRenderSettings keeps the cached frames. Changing the renderer, the prim, or
a setting of the active renderer renders again.

Headlight, off by default, adds a light at the camera to the scene's own
lights. With it off, only the scene's lights light it: a scene with no light
renders its lit surfaces black, and only emission shows.

Render Purpose (on), Proxy Purpose (off) and Guide Purpose (off) choose which
prims of those USD purposes are drawn; prims of the default purpose always are.
With Render Purpose and Proxy Purpose both on, both are drawn. Natron Utility
only rebuilds its list of instances for a change here: geometry already built
for a purpose is kept while that purpose is off.

The output has three layers, and a fourth with the renderers other than Storm
and Natron Utility:

- `rgba`: the colour as the renderer shades it, with the scene's materials and
  lights, in the project's working space with no colour transform. Where nothing was hit it is transparent black.
- `depth.Z`: the distance from the camera along its view axis, in scene units,
  so a plane facing the camera has one depth throughout. Where nothing was hit
  it is +inf.
- `primId.id`: the identity of the prim seen at each pixel, a hash of its path
  (MurmurHash3, 32 bits, as Cryptomatte computes it) whose bits are read as a
  float. Where nothing was hit it is 0. The frame's metadata maps every value to
  its path under the key `primId/` followed by the value's bits as eight
  lowercase hexadecimal digits, for example `primId/1a2b3c4d` = `/World/Box`.
- `normal`: the shading normal in camera space, in `normal.x`, `normal.y` and
  `normal.z`. Where nothing was hit it is 0. Storm and Natron Utility render no
  normal, so the layer is not there with them.

Storm renders whole frames, one at a time, so renders of several USDRender
nodes, or of several frames, take turns on one OpenGL context.

Each USDRender node keeps its own copy of the scene and its Storm renderer, with
the geometry, textures and shaders on the GPU, between renders. A frame whose
scene is unchanged only moves the time, and an edit upstream updates just what
it changed. They are released when the node is deleted, by Cache > Clear All
Caches and Clear Per-Node Cache, by Cache > Free GPU Memory, and, least recently
rendered first, when the nodes' renderers hold more than 2 GB of GPU memory
together. A released node rebuilds them at its next render.

## Subdivision surfaces

Cycles and MoonRay render a mesh as a subdivision surface when their
Subdivision Surfaces setting is on (`cyclesSubdivisionSurfaces`,
`moonraySubdivisionSurfaces` on USDRenderSettings). It is off by default, and
every mesh then renders as its polygons. USD's default subdivision scheme is
Catmull-Clark, so with the setting on, every mesh that does not author a
`subdivisionScheme` of `none` is smoothed. The viewer's refine level has no
effect on either renderer.

- `catmullClark`: a Catmull-Clark surface.
- `loop`: rendered as Catmull-Clark, with a warning that names the mesh.
- `bilinear`: subdivided linearly, which keeps the mesh's shape.
- `none`: never subdivided.

A subdivided mesh shades with its limit surface's normals; its authored
normals are ignored. Creases, corners, holes and the boundary and face-varying
interpolation rules are passed to the renderer.

How finely a surface is diced into micropolygons:

- Cycles: Dicing Rate (`cyclesDicingRate`, 1 by default) is the size of a
  micropolygon edge, in pixels with Adaptive Space (`cyclesAdaptiveSpace`) at
  Pixel, its default, or in scene units at Object. Max Subdivision Level
  (`cyclesMaxSubdivisionLevel`, 12) caps the splitting. Dicing Camera
  (`cyclesDicingCamera`) names the camera prim meshes are diced as seen from;
  empty uses the render camera. A mesh's constant primvars `cycles:dicing_rate`
  (a multiplier on Dicing Rate) and `cycles:max_level` (in place of Max
  Subdivision Level) override them for that mesh.
- MoonRay: Mesh Resolution (`moonrayMeshResolution`, 2) and Adaptive Error
  (`moonrayAdaptiveError`, 0, which turns adaptive dicing off) set the dicing.
  Dicing Camera (`moonrayDicingCamera`) names the camera prim meshes are diced
  as seen from; empty uses the render camera. Faces outside its view get the
  coarsest dicing and its field of view sets how large a face looks, while the
  distance to a face is still measured from the render camera. A mesh's primvars
  `moonray:mesh_resolution` and `moonray:adaptive_error` override them for that
  mesh.

Changing any of these settings re-dices the meshes, in a running render and in
the viewer.
