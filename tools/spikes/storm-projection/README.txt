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
