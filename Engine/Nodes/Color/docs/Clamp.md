# Clamp

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Clamp | exact | `kClampTolerance` | |

Clamp holds its CPU class, exact, except where a divisor, mask or mix below 1
enters, which hold `arithmetic()` (`Tests/Native/NativeClampGpu_Test.cpp`).
