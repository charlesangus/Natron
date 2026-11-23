## Pending

### 2026-11-22T00:00:00Z — info
- refs: M438
- The lens follow-ups for M438 - Lens Distortion In The USD Scene are filed, as F34 - Scene Lens Distortion: M497 - Lens Schema And Camera Authoring, M498 - LensDistortion From Scene, M499 - Scene Lens In Projection Nodes, M500 - USDProject3D Lens Maps, M501 - Lens Maps In The Scene. The user dropped the sketch's D (USDRender Lens Redistort) and G (Natron Utility Distorted Rays) for now, during grilling on 2026-11-22. M438 can close.

### 2026-11-23T16:10:00Z — change-request
- refs: M345
- New milestone, **Scene Camera For Delegate Renders**: Natron never gives a Hydra delegate the scene camera. `HydraRender::Request` carries only a frustum and `HydraRenderImpl.cpp` (~:1546) calls `engine->SetCameraState(view, projection)`, so Cycles and MoonRay render through a free camera with no shutter (and likely no depth-of-field values). Motion blur from a camera's `shutter:open/close` therefore can't work in any Natron render. Fix: add a camera path to the request and call `UsdImagingGLEngine::SetCameraPath` plus `SetFraming`/`SetOverrideWindowPolicy` for camera renders, keeping `SetCameraState` for free-camera viewer navigation; touches framing/conform, engine cache keys and the viewer. Re-enable `VolumeRender{Cycles,MoonRay}.VelocityBlursTheVolume` (disabled in M345 - VDB Rendering Fixes for this reason), with `cycles:integrator:motion_blur` on for Cycles. Found by the M345 gate (run 45155927523: blurred and still renders bit-identical on both renderers).

### 2026-11-23T16:10:00Z — change-request
- refs: M553
- New milestone or task, **Dissolve GPU Smaller-Input Mismatch**: `NativeEdgeInputsGpuTest.DissolveSmallerInput` failed once on the task-graph leg of run 45179223309 (`Tests/Native/NativeEdgeInputsGpu_Test.cpp:270`, 688 mismatches, "Dissolve black: first at float 3920, worst 0.5709780") and passed on the in-job rerun. A 0.57 difference is not rounding; it looks like a read outside the smaller input or uninitialised memory in black edge mode on the GPU path. Possibly the same class as M495 - KeyMix GPU Edge Read Fix; check there first.
