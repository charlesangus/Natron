# M299 - Roto Render Performance

Follow-up from the user's 2026-11-01 UAT of M224 - Fluid Roto And RotoPaint Interaction: handle interaction with many shapes is good now, but Roto and RotoPaint render slowly. Improve the nodes' actual render speed, including caching where it pays (per-shape or per-stroke rasters, reuse across frames for unanimated shapes), building on the cache milestones (M276 - Upstream Reuse On Edits, M300 - Unified Tiered Cache). Profile first: rasterisation, feather, merge tree, and per-render setup.

Deps: M224, M276, M300.

Stub: to be broken down into tasks when claimed.
