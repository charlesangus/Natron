# M257 - Cycles Deep Output

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Render true per-sample deep data from Cycles into a DeepImage, in process, through the transport chosen by M255 - Delegate Output Spikes.

Transport (2026-11-01, user): the delegate fills the host-side deep render-buffer handle M286 - Utility Renderer Deep Output defines (`HydraDeepBuffer`, converted into `DeepImage` by USDRender); no other transport is designed here.

Spike findings to address (M255 - Delegate Output Spikes, P2.T1, `spike/m255/REPORT-P2T1-cycles-deep.md` at 7b386b9f7): the pinned fork already carries Niik-l's deep kernel and a +194/−6 patch hands a `HydraDeepBuffer` out through `GetResource()` when `cycles:deep` is bound (256x256 / 16 spp: 1.27x flat time, 1.3 samples/px mean, 16 max, 43.5 B/px handed out). Problems this milestone must fix: the kernel reserves 2 KB/px (64 fixed slots, about 4.2 GB at 1080p); the absolute merge threshold over-splits curved surfaces; the flatten differs from beauty by up to 0.23; the AOV's flattened pixels came back empty; samples carry no ids.

Blocked on: M255 - Delegate Output Spikes has shown in-process deep is feasible and the user has approved the transport.

Acceptance sketch:
- USDRender with Renderer = Cycles can output a deep image whose samples carry z, zback, colour, alpha.
- Working memory is bounded well below the spike's 2 KB/px, the merge threshold no longer over-splits curved surfaces, flattened deep matches beauty within tolerance, flattened AOVs are filled, and samples carry ids.
- Deep Opacity (USDRender's Deep Content = Opacity Only, M286 - Utility Renderer Deep Output) yields alpha/Z/ZBack-only samples, with the kernel storing no colour or AOV channels.
- The deep image flows through the existing deep cache and RoI path with no side channel.
- A deep comp with DeepMerge and DeepToImage matches a flat render within tolerance.
- Volumes render as deep segments: each sample carries its own z and zback range, so a VDB from M346 - USDVolume Node composites correctly against deep elements through DeepMerge.

## Decisions

- 2026-11-05 — Volume segments are required in Cycles deep output (user): smoke must hold out correctly in a deep comp. M255 - Delegate Output Spikes has to show the transport can carry them; if it can't, bring that back to the user rather than dropping this.
- 2026-11-16 — **Deep opacity output (user, inbox):** Cycles honours the opacity-only channel set. See `PLAN/DECISIONS/2026-11-16-deep-opacity-output.md`.
- 2026-11-16 — **M255 P2.T1 findings folded in (inbox):** the five problems above are this milestone's to fix.
