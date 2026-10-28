# USDProject3D

Projects its Image input through a camera onto whatever the material it authors is
bound to, as Nuke's Project3D does. The output is a material: connect it to a
geometry node's Material input. The projection is per fragment and perspective
correct, so coarse geometry does not warp it. It is a Storm (glslfx) surface: it
shows in the Viewer's 3D view and in USDRender; USDWrite bakes it to UVs and a
`UsdPreviewSurface` for other renderers. The surface is unlit: it shows the image as
it is.

The camera comes from the Camera input, read where it is wired. Camera lists that
scene's cameras; until one is chosen the first is used. A chosen camera the scene
does not have, or no camera at all, is a node error. The camera is stored as values
(a matrix per frame when it moves), so the material keeps projecting where the
camera was wherever it is grafted.

The image's format fills the camera's frame. Data beyond the format (overscan) is
in the texture too: it shows past the frame's edges when Crop is off.

Controls:

- Projection Space: World fixes the projection where the camera is, so geometry
  moving after it slides through the image. Geometry fixes it to the geometry as
  the geometry node it is the material of places it: transforms after that node
  carry the image along. With Use Frame Hold on, the geometry is also taken at
  Frame Hold, so the image sticks to the geometry's own animation too, as Nuke's
  Project3D with a held camera does. Only the primitive nodes (USDCard, USDCube,
  USDSphere, USDCylinder) place a projection; bound any other way, a Geometry
  projection takes the camera in the geometry's own coordinates.
- Project On: Both, Front (faces facing the camera) or Back.
- Crop: beyond the camera's frame the surface shows Outside, and inside it, where the
  image has no data, black. Off, the image's data beyond its format shows there, and
  its edge pixels extend beyond that. A baked export cannot crop, so it shows the
  overscan, with a warning.
- Clip Near Far: nearer than the camera's near plane or beyond its far one, the
  surface shows Outside.
- Outside: Black, or Transparent (not drawn). Behind the camera is always Outside.
- Fit Mode: how the camera's aperture is conformed to the image's format, as in
  USDRender.
- Use Frame Hold, Frame Hold: project through the camera as it is at Frame Hold on
  every frame. The image still plays.

There is no occlusion: the image falls on every face in the frustum, hidden ones
included. Without an Image input the projection is black, with a warning.
