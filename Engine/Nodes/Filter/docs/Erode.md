# Erode

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Erode, Dilate | exact | `kTolerance` | rectangular min and max are exact in float |

Erode and Dilate hold exact, bit for bit on NaNs, signed zeros and denormals (`Tests/Native/NativeErodeDilateGpu_Test.cpp`).
