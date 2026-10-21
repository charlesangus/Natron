# Add

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Add | arithmetic | `kAdd` (`NativeColorMath_Test`) | |

Add and Multiply hold `arithmetic()` and Gamma holds `transcendental()`, where a subnormal input may come back as 0 on a device that flushes denormals (`Tests/Native/NativeColorMathGpu_Test.cpp`).
