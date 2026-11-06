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
The other delegates (Cycles, MoonRay) render on the CPU.

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

The output has three layers:

- `rgba`: the colour as the renderer shades it, with the scene's materials and lights,
  in the project's working space with no colour transform. Where nothing was
  hit it is transparent black.
- `depth.Z`: the distance from the camera along its view axis, in scene units,
  so a plane facing the camera has one depth throughout. Where nothing was hit
  it is +inf.
- `primId.id`: the identity of the prim seen at each pixel, a hash of its path
  (MurmurHash3, 32 bits, as Cryptomatte computes it) whose bits are read as a
  float. Where nothing was hit it is 0. The frame's metadata maps every value to
  its path under the key `primId/` followed by the value's bits as eight
  lowercase hexadecimal digits, for example `primId/1a2b3c4d` = `/World/Box`.

Storm renders whole frames, one at a time, so renders of several USDRender
nodes, or of several frames, take turns on one OpenGL context.

Each USDRender node keeps its own copy of the scene and its Storm renderer, with
the geometry, textures and shaders on the GPU, between renders. A frame whose
scene is unchanged only moves the time, and an edit upstream updates just what
it changed. They are released when the node is deleted, by Cache > Clear All
Caches and Clear Per-Node Cache, by Cache > Free GPU Memory, and, least recently
rendered first, when the nodes' renderers hold more than 2 GB of GPU memory
together. A released node rebuilds them at its next render.
