# M256 - Delegate AOV Selection

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Choose which AOVs and light-path expressions USDRender renders for Cycles and MoonRay, and map each to a Natron layer and channels. The mechanism (delegate-discovered checklist with custom LPE strings, or USD RenderSettings prims) is chosen by M255 - Delegate Output Spikes. Natron Utility is its first full client (2026-11-01, user): M285 - Utility Renderer AOVs depends on this milestone and delivers that delegate's full AOV list, including per-AOV metadata for Cryptomatte manifests, through the mechanism chosen here.

Blocked on: M255 - Delegate Output Spikes has recommended an AOV mechanism and the user has approved it.

Acceptance sketch:
- The delegates' AOV lists are reachable from a checklist or from authored RenderSettings, as the spike decides.
- Each selected AOV appears as a named Natron layer.
- Unselected AOVs cost no render time.
- 2026-11-03 — AOVs are authored as RenderProduct/RenderVar under the /Render prim through USDRenderSettings (M253.P2), read with M253.P2.T3's library (UsdRenderComputeSpec), and hashed through M253.P2.T8's side table (user decision on M253, 2026-11-03).
