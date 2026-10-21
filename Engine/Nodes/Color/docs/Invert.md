# Invert

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Invert | exact | `kInvertTolerance` | |

Invert holds its CPU class, exact, except where a divisor, mask or mix below 1
enters, which hold `arithmetic()` (`Tests/Native/NativeInvertGpu_Test.cpp`).
