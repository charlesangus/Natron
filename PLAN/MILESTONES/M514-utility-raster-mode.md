# M514 - Utility Raster Mode

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A raster render mode in Natron Utility for speed in simple setups, mostly projections with motion blur (F7 - Natron Utility Core UAT round 1, item 6). Primary visibility is rasterised on the GPU through M472 - GPU Raster Pipelines into a visibility buffer, and secondary effects (shadows, reflections, refraction) are ray-traced from it with Utility's existing tracer. Motion blur and anti-aliasing come from accumulating sub-frames along Utility's shutter segments, each with a sub-pixel jitter. Without a capable GPU, the mode renders through the ray path and warns.

Blocked on: F7 - Natron Utility Core landing on `main`. Its GPU path (batch sizing, VRAM budget, instance splitting, shutter-segment motion blur) is still changing in UAT round 2, and this mode builds on that code.

Acceptance sketch:
- A Render Mode choice (Ray / Raster) on the Utility render settings; Raster renders a projected-card scene with motion blur and the same AOV set as Ray mode.
- Shadows, reflections and refraction appear in Raster mode, ray-traced from the rasterised primary hits.
- Without a GPU, or without M472's `rasterFloatBlend`, Raster mode produces the Ray image and a node warning.
- Renders cancel fast between sub-frames.

## Scope

**In:**
- Raster primary visibility, ray-traced secondary effects, sub-frame accumulation with jitter, the Ray-path fallback.

**Out:**
- Storm oversampling and motion blur: M515 - Storm Oversample And Motion Blur.
- A CPU rasteriser: none (the fallback is the Ray path).
- A velocity-based post blur: none.

**Edge cases:**
- id and depth AOVs are never averaged across sub-frames; they take one sample's value.
- A low sub-frame count steps visibly; the count is a control.

**Assumptions:**
- Unconfirmed: the sub-frame count defaults high enough for smooth blur on a typical projection shot, and the default value is settled at elaboration.

## Decisions

- 2026-11-22 — A raster mode inside Natron Utility, not a new delegate (user, grilling): one renderer to maintain, sharing scene translation, AOVs and settings. Storm also gets oversampling and shutter accumulation, in M515 - Storm Oversample And Motion Blur.
- 2026-11-22 — Motion blur by accumulating time samples along the shutter segments (user, grilling): exact, and cheap per sample on raster.
- 2026-11-22 — Raster primary, ray secondary (user, grilling): the speed of raster with the ray look.
- 2026-11-22 — Without a usable GPU, render through the Ray path and warn (user, grilling): no CPU rasteriser to maintain.
- 2026-11-22 — Anti-aliasing by sub-pixel jitter on each accumulation sample; Raster mode does not have to match Ray mode, at most a loose parity check (user, grilling).
