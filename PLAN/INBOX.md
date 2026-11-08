## Pending

### 2026-11-07T14:00:00-05:00 — change-request
- refs: none
- User asked for a new milestone under F9 - Delegate Projection: cancel and stop responsiveness for Cycles and MoonRay. Viewers: an old render can run on until the next sync; changing renderer or settings joins the session under the stage lock and can freeze the UI, and the engine may only be destroyed under that lock. Offline: cancel is only checked between renders, and with no abort callback a stop can take up to 600 s. Goal: every renderer cancels quickly, and the UI never waits on a stop.

### 2026-11-07T14:00:00-05:00 — change-request
- refs: none
- User asked for a new standalone milestone: Cycles, MoonRay and Natron Utility viewers use the viewer's display transform (OCIO view, gain, gamma, lighting mode) as Storm does. Progressive frames are 8-bit sRGB from `srgbTable()` today, so frames must carry float, and it needs a design pass. This also answers the display transform question logged under M254 - Progressive Viewer Render on the board.

### 2026-11-07T14:20:00-05:00 — change-request
- refs: none
- User asked for one follow-up milestone: a direct material binding on native instance roots, so projection reaches native instanced prims. It covers rigid fallback under M341 - Per-Placement Geometry Projection and M342 - Sticky Projections On Deforming Geometry, which show no projection and warn today. Belongs with F9 - Delegate Projection unless the planner has a reason otherwise.

### 2026-11-07T14:40:00-05:00 — change-request
- refs: none
- User wants the mixed pixel aspect tile bug fixed as its own milestone. A tile with source pixel aspect 1 and output pixel aspect 2 reads black outside its requested area unless a larger cache entry exists, so its result varies with cache contents. Reported under M300 - Unified Tiered Cache, predates it. User's framing: pixel aspect is display metadata, a pixel is still a pixel, so question why RoI or cache keys convert by it at all before patching. The fix may change the documented GPU contract.

### 2026-11-07T15:30:00-05:00 — change-request
- refs: none
- User asked for a new milestone: apply USD camera exposure (`exposure`, and the related fStop, shutter and ISO terms as USD specifies) in every renderer delegate, USDRender and the viewer. Lobby's cameras author `exposure = -10`, nothing reads it, and Cycles frames average about 8,000. Answers the M251 - Cycles Rendering camera exposure question on the board.

