# M110 - Remaining Native GPU Ports

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Port the native nodes M85 left on the CPU, using M85's description, wrap helpers and resampler:
- EdgeDetect, on M85's Blur line filters;
- Reformat, on M85's `resample.slang`;
- Crop with softness, and Position;
- Dissolve, as a multi-input point op;
- Shuffle, ShuffleCopy, AddLayers and RemoveLayers, once regions accept multi-plane tasks (M109).

Each gets a CPU/GPU parity test with its README tolerance.

Blocked on: M85 merged (the description, `pixelwrap.slang` and `resample.slang`), and for the channel nodes, M109's multi-plane placement.

Acceptance sketch:
- Each ported node matches its CPU output within its stated tolerance.
- A Read → Reformat → Grade → Shuffle → Write chain on RADV runs as one region between the Read and the Write.
