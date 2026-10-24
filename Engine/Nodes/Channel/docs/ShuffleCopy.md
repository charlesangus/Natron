# ShuffleCopy

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ShuffleCopy | exact | `kShuffleGpuTolerance` | |

ShuffleCopy holds exact on the GPU, as Shuffle does: both executors run one channel route, which
copies channels of input 2 and input 1 and writes the constants 0 and 1. Its bbox choice can make
the output reach past an input's region, and those pixels read 0 there as on the CPU, as does a
disconnected input. A plane stays on the CPU when a channel the mapping reads is missing or an input
it reads is not float (`Tests/Native/NativeShuffleGpu_Test.cpp`).
