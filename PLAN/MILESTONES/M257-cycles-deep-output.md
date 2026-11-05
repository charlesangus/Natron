# M257 - Cycles Deep Output

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Render true per-sample deep data from Cycles into a DeepImage, in process, through the transport chosen by M255 - Delegate Output Spikes.

Transport (2026-11-01, user): the delegate fills the host-side deep render-buffer handle M286 - Utility Renderer Deep Output defines (`HydraDeepBuffer`, converted into `DeepImage` by USDRender); no other transport is designed here.

Blocked on: M255 - Delegate Output Spikes has shown in-process deep is feasible and the user has approved the transport.

Acceptance sketch:
- USDRender with Renderer = Cycles can output a deep image whose samples carry z, zback, colour, alpha.
- The deep image flows through the existing deep cache and RoI path with no side channel.
- A deep comp with DeepMerge and DeepToImage matches a flat render within tolerance.
- Volumes render as deep segments: each sample carries its own z and zback range, so a VDB from M346 - USDVolume Node composites correctly against deep elements through DeepMerge.

## Decisions

- 2026-11-05 — Volume segments are required in Cycles deep output (user): smoke must hold out correctly in a deep comp. M255 - Delegate Output Spikes has to show the transport can carry them; if it can't, bring that back to the user rather than dropping this.
