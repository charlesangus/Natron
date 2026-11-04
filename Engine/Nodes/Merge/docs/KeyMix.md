# KeyMix

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| KeyMix | arithmetic | `ParityTolerance::arithmetic()` | |

KeyMix copies A into B's processed channels, and the host's mask and mix stage blends that over B,
so A shows where the mask is white. Its GPU step returns A unchanged; the wrap's mask blend is the
CPU stage's `v * alpha + (1 - alpha) * src`, held to arithmetic (1e-6 relative) on the GPU
(`Tests/Native/NativeKeyMixGpu_Test.cpp`). KeyMix shows A through the mask, so without a mask
applied (the Mask input unconnected, or its enable unticked) it does nothing: the node routes to B,
with B's region and format, and renders nothing. Without A it also routes to B, and without B in
either case it is black. `setBBox` picks the output region when A shows through a mask: the union
of A and B, B's or A's.
