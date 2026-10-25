# RemoveLayers

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| RemoveLayers | exact | `kLayersGpuTolerance` | |

RemoveLayers holds exact on the GPU, as AddLayers does: both executors run one channel route, which
copies the kept colour channels and writes 0 on the others. Only a narrowed colour plane is
rendered. An unchanged colour plane and the non-colour layers the node keeps pass through, so in a
GPU region they are identity members, and a dropped plane or layer is not produced. A plane stays on
the CPU when the input is not float, since the CPU converts it
(`Tests/Native/NativeLayersGpu_Test.cpp`).
