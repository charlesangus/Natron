# M110 - Remaining Native GPU Ports

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Port the native nodes M85 left on the CPU, using M85's description, wrap helpers and resampler:
- EdgeDetect, on M85's Blur line filters;
- Reformat, on M85's `resample.slang`;
- Crop with softness, and Position;
- Dissolve, as a multi-input point op;
- Multi-plane placement candidates: a multi-plane `GpuQuery`/`GpuRenderArgs` in `Engine/Nodes/Image/GpuRender.h`, so Shuffle-family nodes can join regions. M109 - GPU Region Engine Hardening moved this out of its scope. It edits the header M126 - Runtime GPU Shaders changed, so it starts from M126's merged version;
- Shuffle, ShuffleCopy, AddLayers and RemoveLayers, on that multi-plane placement.

Each gets a CPU/GPU parity test with its README tolerance.

Each port is a GPU op body on M137 - Native Base Class Flexibility's framework: a Point or Composite `IPixelOp`, a Passes pass list or a Resample op, recorded by `recordStagesGpu`. None is a hand-written `renderGpu` (user, M137 grill, 2026-10-19). By then M137 has migrated these nodes' CPU paths.

Blocked on: M137 - Native Base Class Flexibility merging, and M109 - GPU Region Engine Hardening merging, since this milestone's region and placement work builds on its hardened engine. M85 - Native GPU Kernels and M126 - Runtime GPU Shaders have merged.

Acceptance sketch:
- Each ported node matches its CPU output within its stated tolerance.
- A Read → Reformat → Grade → Shuffle → Write chain on RADV runs as one region between the Read and the Write.
