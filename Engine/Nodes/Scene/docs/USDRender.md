# USDRender

Renders its scene input through one of the scene's cameras with a Hydra render
delegate into an image. Renderer chooses the delegate from those installed:

- Storm, USD's OpenGL renderer and the default. It needs an OpenGL 4.5
  compatibility context: in the application it shares the viewer's OpenGL, and
  without a window system (`NatronRenderer`, a server without X) it makes its own
  through EGL, which Mesa's llvmpipe software renderer provides. Where neither is
  available the node reports why instead of rendering; image-only graphs are
  unaffected.
- Natron Utility, Natron's own renderer. It draws on the CPU with no lighting:
  `rgba` is each prim's display colour, and `depth.Z` and `primId.id` match
  Storm's. Cancelling stops it between tiles; a change of scene being taken in or
  a BVH being built finishes first.
- Any other installed delegate, which renders on the CPU on its own threads.

Camera lists the cameras of the input scene by path. Until one is chosen the
scene's first camera is used. A chosen camera that the scene no longer has is
an error naming its path, rather than a silent switch to another camera.

Format sets the size and pixel aspect ratio of the image, the project's format
by default. Fit Mode sets how the camera's aperture is conformed to the format
when their shapes differ:

- Fill (the default) fits the aperture inside the format's shape by cropping
  the side that does not fit.
- Horizontal keeps the aperture's width and crops or extends its height.
- Vertical keeps the aperture's height and crops or extends its width.

Headlight, off by default, adds a light at the camera to the scene's own
lights. With it off, only the scene's lights light it: a scene with no light
renders its lit surfaces black, and only emission shows.

Render Purpose (on), Proxy Purpose (off) and Guide Purpose (off) choose which
prims of those USD purposes are drawn; prims of the default purpose always are.
With Render Purpose and Proxy Purpose both on, both are drawn. Natron Utility
only rebuilds its list of instances for a change here: geometry already built
for a purpose is kept while that purpose is off.

The output has three layers:

- `rgba`: the colour as the renderer shades it, under Storm with the scene's
  materials and lights, in the project's working space with no colour transform.
  Where nothing was hit it is transparent black.
- `depth.Z`: the distance from the camera along its view axis, in scene units,
  so a plane facing the camera has one depth throughout. Where nothing was hit
  it is +inf.
- `primId.id`: the identity of the prim seen at each pixel, a hash of its path
  (MurmurHash3, 32 bits, as Cryptomatte computes it) whose bits are read as a
  float. Where nothing was hit it is 0. The frame's metadata maps every value to
  its path under the key `primId/` followed by the value's bits as eight
  lowercase hexadecimal digits, for example `primId/1a2b3c4d` = `/World/Box`.

Storm renders whole frames, one at a time: renders of several USDRender nodes,
or of several frames, take turns on one OpenGL context.

Each USDRender node keeps its own copy of the scene and its Storm renderer, with
the geometry, textures and shaders on the GPU, between renders. A frame whose
scene is unchanged only moves the time, and an edit upstream updates just what
it changed. They are released when the node is deleted, by Cache > Clear All
Caches and Clear Per-Node Cache, by Cache > Free GPU Memory, and, least recently
rendered first, when the nodes' renderers hold more than 2 GB of GPU memory
together. A released node rebuilds them at its next render.
