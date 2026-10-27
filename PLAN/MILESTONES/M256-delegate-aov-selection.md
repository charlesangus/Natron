# M256 - Delegate AOV Selection

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Choose which AOVs and light-path expressions USDRender renders for Cycles and MoonRay, and map each to a Natron layer and channels. The mechanism (delegate-discovered checklist with custom LPE strings, or USD RenderSettings prims) is chosen by M255 - Delegate Output Spikes.

Blocked on: M255 - Delegate Output Spikes has recommended an AOV mechanism and the user has approved it.

Acceptance sketch:
- The delegates' AOV lists are reachable from a checklist or from authored RenderSettings, as the spike decides.
- Each selected AOV appears as a named Natron layer.
- Unselected AOVs cost no render time.
