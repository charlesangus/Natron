## Pending

### 2026-11-07T14:20:00-05:00 — change-request
- refs: none
- User asked for one follow-up milestone: a direct material binding on native instance roots, so projection reaches native instanced prims. It covers rigid fallback under M341 - Per-Placement Geometry Projection and M342 - Sticky Projections On Deforming Geometry, which show no projection and warn today. Belongs with F9 - Delegate Projection unless the planner has a reason otherwise.

### 2026-11-07T14:40:00-05:00 — change-request
- refs: none
- User wants the mixed pixel aspect tile bug fixed as its own milestone. A tile with source pixel aspect 1 and output pixel aspect 2 reads black outside its requested area unless a larger cache entry exists, so its result varies with cache contents. Reported under M300 - Unified Tiered Cache, predates it. User's framing: pixel aspect is display metadata, a pixel is still a pixel, so question why RoI or cache keys convert by it at all before patching. The fix may change the documented GPU contract.

### 2026-11-07T15:30:00-05:00 — change-request
- refs: none
- User asked for a new milestone: apply USD camera exposure (`exposure`, and the related fStop, shutter and ISO terms as USD specifies) in every renderer delegate, USDRender and the viewer. Lobby's cameras author `exposure = -10`, nothing reads it, and Cycles frames average about 8,000. Answers the M251 - Cycles Rendering camera exposure question on the board.
