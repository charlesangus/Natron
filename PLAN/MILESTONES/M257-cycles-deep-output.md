# M257 - Cycles Deep Output

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Render true per-sample deep data from Cycles into a DeepImage, in process, through the transport chosen by M255 - Delegate Output Spikes.

Blocked on: M255 - Delegate Output Spikes has shown in-process deep is feasible and the user has approved the transport.

Acceptance sketch:
- USDRender with Renderer = Cycles can output a deep image whose samples carry z, zback, colour, alpha.
- The deep image flows through the existing deep cache and RoI path with no side channel.
- A deep comp with DeepMerge and DeepToImage matches a flat render within tolerance.
