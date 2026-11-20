# USDProject3D

Projects its Image input through a camera onto whatever the material it authors is
bound to. The output is a material: connect it to a geometry node's Material
input, to USDAssignMaterial's Material input to project onto the prims a pattern
names, or bind it by its path as any material in the scene is bound (a collection
binding included). The projection is per fragment and perspective
correct, so coarse geometry does not warp it. It is a Storm (glslfx) surface: it
shows in the Viewer's 3D view and in USDRender; USDWrite bakes it to UVs and a
`UsdPreviewSurface` for other renderers, and warns about a material it cannot bake:
one bound to no geometry, bound to a gprim that is not a mesh, or inside an
instance. The image is always rendered in the project's first view. The surface is unlit and emissive: it shows
the image as it is, whatever lights the scene has. The image's alpha is its
opacity, as with USDEmissiveMaterial's Use Alpha on: the surface shows the image as
it would be composited over what is behind it, so where the image is transparent
the geometry and scene behind show through. Outside the frame with Crop on and
beyond the image's data it is transparent. A transparent surface is drawn in the
renderer's translucent pass, which writes no depth or prim id.

The camera comes from the Camera input, read where it is wired, at Camera Path.
Camera lists that scene's cameras and picks Camera Path from them; Camera Path
can also name a camera that is not there yet, listed as missing. Until the Camera
input has a camera at that path the projection shows nothing, with a warning, and
it projects as soon as one arrives. An empty Camera Path uses the first camera; no
camera at all then is a node error, as is a Camera Path that is not an absolute
prim path. The camera is stored as values
(a matrix per frame when it moves), so the material keeps projecting where the
camera was wherever it is grafted.

The image's format fills the camera's frame. The projection's texture, like a
material's live texture, holds the format and the image's data beyond it: data
beyond the format (overscan) shows past the frame's edges when Crop is off.

Controls:

- Projection Space: World fixes the projection where the camera is, so geometry
  moving after it slides through the image. Geometry fixes it to the geometry as
  the node it is connected to places it: transforms after that node carry the image
  along.

  Each prim bound to the material has its own placement, so one projection can lie
  on prims at different places, each showing the image where the camera sees it.
  The primitive nodes (USDCard, USDCube, USDSphere, USDCylinder) place what they
  create, and USDAssignMaterial places the geometry it matches, whether the
  projection comes from its Material input or its Material Path, where that geometry
  is at that node.

  With Use Frame Hold on, the geometry is also taken at Frame Hold, so the image
  sticks to the geometry's own animation too, and through USDAssignMaterial to
  points that move by themselves: time-sampled meshes, including Alembic caches,
  skinned UsdSkel meshes, curves and points. Each such prim gets a primvar per hold
  frame, natronReferencePointsHold followed by the frame, of its world-space points
  at Frame Hold as they are at that node; the image, Project On and Clip Near Far are
  all judged on that surface. A skinned mesh's points are its skinned result at
  Frame Hold. A rest-position primvar on the prim is ignored: the image lines up with
  the camera at the hold, not with a modelled pose. Frame Hold outside a cache's
  range reads USD's nearest sample. Prims whose points do not move and are not
  skinned get no reference-points primvar, only their placement.

  Projections at one Frame Hold share a prim's placement. If a prim changes between
  two assigns of different projections at one Frame Hold, as in USDAssignMaterial,
  USDEditTransform, USDAssignMaterial, it can only keep one placement: the later
  assign's wins, so the two projections cannot both be correct, and that assign
  warns, naming a few of the prims and how many there are. A prim bound to the
  earlier projection for another purpose then shows it misplaced. Assigning one
  projection twice, or projections that agree on the prim's placement, gives no
  warning. To keep both, assign them before the transform or hold them at different
  frames.

  Limits: a prim whose topology changes over the project's frame range, or that USD
  cannot skin, stays rigid with a warning naming it, and slides. Bound any other way
  than by those nodes, such as in a file or by a script, a Geometry projection takes
  the camera in the geometry's own coordinates, and with no node to place it,
  deforming geometry slides through the image. Native instances project from
  their own placements, as USDAssignMaterial binds them on their instance roots. A
  gprim offset from its instance root projects shifted, and a sticky or held projection
  on a deforming instanced mesh stays rigid, each with a warning. The instances of a
  point instancer share their prototype's placement, with a warning: each shows the
  image as it falls on the prototype. A baked export cannot bake instanced projections,
  as overs cannot reach below an instance and instances share the prototype's UVs: it
  keeps the Storm shader and warns.
- Project On: Both, Front (faces facing the camera) or Back. Curves and points have no
  faces, so they show as Both in Storm and Cycles. MoonRay's shading cannot tell them
  from meshes, so there they follow Project On by their shading normal. A baked export
  ignores it, with a warning.
- Crop: beyond the camera's frame the surface shows Outside, and inside it, where the
  image has no data, black. Off, the image's data beyond its format shows there, and
  its edge pixels extend beyond that. A baked export cannot crop, so it shows the
  overscan, with a warning.
- Clip Near Far: nearer than the camera's near plane or beyond its far one, the
  surface shows Outside. A baked export ignores it, with a warning.
- Outside: Black, or Transparent (not drawn). Behind the camera is always Outside.
  A baked export shows Black as transparent beyond the image's data: the baked
  texture's border is clear. Set to anything else, it warns that it is ignored.
- Fit Mode: how the camera's aperture is conformed to the image's format, as in
  USDRender.
- Use Frame Hold, Frame Hold: project through the camera as it is at Frame Hold on
  every frame. The image still plays.

There is no occlusion: the image falls on every face in the frustum, hidden ones
included. Without an Image input the projection is black, with a warning. If the
node on the Image input was deleted, the projection is magenta, with a warning, until
something is connected to the input or the deletion is undone.
