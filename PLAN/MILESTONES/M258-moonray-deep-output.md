# M258 - MoonRay Deep Output

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Render true per-sample deep data from MoonRay into a DeepImage. MoonRay stays out of process: deep travels over a new banded DeepFrame Arras message into the host-side deep buffer (see Decisions, 2026-11-16).

Transport (2026-11-01, user): the delegate fills the host-side deep render-buffer handle M286 - Utility Renderer Deep Output defines (`HydraDeepBuffer`, converted into `DeepImage` by USDRender); no other transport is designed here.

Shape of the work (2026-11-16 decision): fork mcrt_messages and mcrt_computation; add a DeepFrame message, sent after the FINISHED progressive frame and before `stopFrame`, serialised from `RenderContext::getDeepBuffer()` in row bands so no message nears the 4 GiB buffer limit or the chunking copy; hdMoonray receives it into the `HydraDeepBuffer` returned from `HdRenderBuffer::GetResource()`. Estimated 15–22 days and 900–1,400 fork lines across moonray, mcrt_messages, mcrt_computation and hdMoonray — expect to split into several milestones (or a Feature-sized set) when elaborated.

Blocked on: M255 - Delegate Output Spikes finishing its verification of the Arras route (M255.P2.T2), including whether it carries volume segments.

Acceptance sketch:
- USDRender with Renderer = MoonRay can output a deep image whose samples carry z, zback, colour, alpha.
- The deep image flows through the existing deep cache and RoI path with no side channel.
- A deep comp matches a flat render within tolerance.
- Deep Opacity (USDRender's Deep Content = Opacity Only, M286 - Utility Renderer Deep Output) yields alpha/Z/ZBack-only samples, and the DeepFrame bands then carry only those channels, cutting Arras transfer.
- Deep is produced for the final frame only, never for progressive passes; a cancel during the deep transfer stops promptly.
- Volumes render as deep segments: each sample carries its own z and zback range, so a VDB from M346 - USDVolume Node composites correctly against deep elements through DeepMerge.

## Decisions

- 2026-11-05 — Volume segments are required in MoonRay deep output (user): smoke must hold out correctly in a deep comp. M255 - Delegate Output Spikes has to show the transport can carry them; if it can't, bring that back to the user rather than dropping this.
- 2026-11-16 — MoonRay deep travels over Arras, MoonRay stays out of process (user): a banded DeepFrame Arras message after the FINISHED frame, received by hdMoonray into `HydraDeepBuffer`; deep is final-frame only and multi-machine merge (`progmcrt_merge`) gets no deep. Rejected: in-process rendering for deep (loses crash/memory isolation and concurrent renders; one global RenderDriver; process-wide TBB cap), a deep EXR side output read back from disk (half-float AOVs, temp files, delayed cancel), and a separate batch render (double cost, deep would not match the flat AOVs). Full record: PLAN/DECISIONS/2026-11-16-moonray-deep-over-arras.md.
- 2026-11-16 — **Deep opacity output (user, inbox):** MoonRay honours the opacity-only channel set, and the DeepFrame bands omit colour and AOVs when it is set. See `PLAN/DECISIONS/2026-11-16-deep-opacity-output.md`.
