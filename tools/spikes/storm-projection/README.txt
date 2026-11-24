Throwaway spike: a glslfx camera-projection surface in Storm.

Run in natron-dev (writes build/m234-spike/):
    tools/spikes/storm-projection/run-llvmpipe.sh

gen.py writes scene.usda: a 4x4 single-quad card tilted 50 degrees, a second card facing away from
the projector, an animated projector camera, a view camera, and two materials whose surface is
Engine/Nodes/Scene/shaders/natronProject3D.glslfx (info:implementationSource = sourceAsset,
info:glslfx:sourceAsset). The plate is a UsdUVTexture connected to the shader's float4 "plate"
input. usdrecord renders frames 1 and 2 through Storm on llvmpipe.

Findings (USD 0.26.8):
- sdrGlslfx reads the configuration's "parameters": 16 numbers -> matrix (GfMatrix4d), 4 -> float4,
  an integer -> int. "textures" entries come out as color3f, so the plate is a float4 parameter
  connected to the texture's rgba output instead.
- Storm generates HdGet_<param>(vec2 coord) for a parameter connected to a texture node, so the
  shader samples at its own projected coordinate. The texture node still needs an st input.
- The surface stage gets Peye; GetWorldToViewInverseMatrix() gives the world position and
  GetWorldToViewMatrix() moves the projector into eye space. IsFlipped() and gl_FrontFacing with
  the screen-space geometric normal give the face's side whatever doubleSided is.
- Time-sampled matrix, float4 and int inputs all animate (frames 1 and 2 differ as expected).
- Entry points: surfaceShader(vec4 Peye, vec3 Neye, vec4 color, vec4 patchCoord) and
  displacementShader(int index, vec4 Peye, vec3 Neye, vec4 patchCoord).

A frame per prim, one material (writes build/m341-spike/):
    tools/spikes/storm-projection/run-placement.sh <name>:<mode>:<instances>[:<glslfx>] ...

gen_placement.py writes cards apart and moving. Modes: "material" gives each card its own material
(the reference), "reader" shares one material whose geometryFrame comes from a constant matrix primvar
through UsdPrimvarReader_matrix, "metadata" names the primvar in the glslfx "primvars" metadata. Instances: 0
none, 1 binding inside the prototype, 2 collection binding on an ancestor, 3 direct binding on the
instance root.

Findings:
- reader renders pixel-identical to material at frames 1 and 2 outside the deliberately bare card.
  Held and time-sampled primvars both read correctly; a card without the primvar gets the reader's
  fallback; the glslfx code does not change. metadata was not run: it would need #ifdef HD_HAS_ in the
  shader for the fallback that reader gets from Storm.
- A primvar name may carry a hold frame (natronProjectionFrameHold1_5): same pixels as the plain name.
- On a native instance root the primvar gives each instance its own placement, with instances 1 or 3.
- With instances 2 the instances draw lit grey whatever the material: Storm does not take a collection
  binding from an ancestor into an instance, and that is the form USDAssignMaterial authors.

Reference points on deforming prims:

run-sticky.sh renders gen_sticky.py's scene at frames 1 and 2 in one held Geometry material: a deforming
mesh, linear and cubic curves, points and a rigid moving card. "reader" reads a vertex primvar of each
prim's world-space points at the hold through UsdPrimvarReader_float3; "baked" has st baked from the same
points under a plain textured surface. compare_sticky.py counts where they differ, per prim.
SPIKE_PROJECT_ON sets Project On, SPIKE_BARE_WIDTHS drops authored widths and subdivides the mesh,
SPIKE_COMPLEXITY sets usdrecord's complexity.

Findings:
- The checker stays on the same part of every deforming prim at both frames, and the rigid card beside
  them takes its frame primvar in the same material. The reader's fallback, 1e30, marks a prim without the
  primvar; HD_HAS_referencePoints is defined either way, so the preprocessor cannot tell them apart.
- The two modes differ along checker edges and on the point sprites: 2839 pixels of 31795 drawn by both at
  frame 1, 1940 of 31393 at frame 2. The bake interpolates its UVs across a quad and colours a whole
  sprite from one UV, where the surface projects per fragment. No region of the image is displaced. The
  white strip on the bake's card is the plate's white corner, which the surface discards as out of frame.
- IsFlipped() and ApplyInstanceTransformInverse() are defined for meshes only. A surface calling them
  does not link on curves or points, and those prims draw grey, in World space too. HD_HAS_ptexFaceOffset
  is defined after the surface, and HD_HAS_widths only where widths are authored, so nothing in the
  preprocessor tells the prim types apart. The surface carries its own flip test and reads the points
  primvar for the position on the object.
- At run time, fwidth(patchCoord.x) is zero on curves and points, whose patch coordinate has no u, and
  not on mesh faces. Back hides the mesh and card and leaves curves and points showing, at low and high
  complexity.
