## Pending

### 2026-11-23T16:10:00Z — change-request
- refs: M553
- New milestone or task, **Dissolve GPU Smaller-Input Mismatch**: `NativeEdgeInputsGpuTest.DissolveSmallerInput` failed once on the task-graph leg of run 45179223309 (`Tests/Native/NativeEdgeInputsGpu_Test.cpp:270`, 688 mismatches, "Dissolve black: first at float 3920, worst 0.5709780") and passed on the in-job rerun. A 0.57 difference is not rounding; it looks like a read outside the smaller input or uninitialised memory in black edge mode on the GPU path. Possibly the same class as M495 - KeyMix GPU Edge Read Fix; check there first.
