# KeyMix

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| KeyMix | arithmetic | `ParityTolerance::arithmetic()` | |

KeyMix copies A into B's processed channels, and the host's mask and mix stage blends that over B,
so A shows where the mask is white. Its GPU step returns A unchanged; the wrap's mask blend is the
CPU stage's `v * alpha + (1 - alpha) * src`, held to arithmetic (1e-6 relative) on the GPU
(`Tests/Native/NativeKeyMixGpu_Test.cpp`). Without A the node routes to B, and without either
input it is black. `setBBox` picks the output region: the union of A and B, B's or A's.
