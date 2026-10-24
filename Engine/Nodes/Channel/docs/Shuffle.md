# Shuffle

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Shuffle | exact | `kShuffleGpuTolerance` | |

Shuffle holds exact on the GPU: the CPU and the GPU both run the node's channel route, which only
copies channels and writes the constants 0 and 1, so nothing is computed. A colour bit no output
view writes passes through from the input's colour plane, and a colour channel the input lacks reads
0. A plane stays on the CPU when a channel the mapping reads is missing, so that the CPU reports it,
or when an input it reads is not float, which the CPU reads as 0
(`Tests/Native/NativeShuffleGpu_Test.cpp`).
