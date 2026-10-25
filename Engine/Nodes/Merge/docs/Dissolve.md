# Dissolve

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Dissolve | arithmetic | `kDissolveTolerance` | |

Dissolve holds its CPU class, arithmetic (1e-6 relative), on the GPU: the kernel is the CPU step's
`(to - from) * blend + from` with the multiply and add unfused, and only a fractional `which`
reaches it, since a whole `which` is an identity routed to its input
(`Tests/Native/NativeDissolveGpu_Test.cpp`).
