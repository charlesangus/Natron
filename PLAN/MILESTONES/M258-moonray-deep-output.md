# M258 - MoonRay Deep Output

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Render true per-sample deep data from MoonRay into a DeepImage, in process, through the transport chosen by M255 - Delegate Output Spikes.

Blocked on: M255 - Delegate Output Spikes has shown MoonRay can produce in-process deep and the user has approved the transport.

Acceptance sketch:
- USDRender with Renderer = MoonRay can output a deep image whose samples carry z, zback, colour, alpha.
- The deep image flows through the existing deep cache and RoI path with no side channel.
- A deep comp matches a flat render within tolerance.
