# AddLayers

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| AddLayers | exact | `kLayersGpuTolerance` | |

AddLayers holds exact on the GPU: the CPU and the GPU both run the node's channel route, which only
copies the input's colour channels and writes 0, so nothing is computed. A widened colour plane
takes each colour bit the input has from the input's colour plane and 0 on the others, and an added
non-colour layer is all 0. A layer the input already carries passes through, so in a GPU region it
is an identity member. A plane stays on the CPU when the input is not float, since the CPU converts
it (`Tests/Native/NativeLayersGpu_Test.cpp`).
