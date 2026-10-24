# M165 - Rich Viewer Overlays

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Let nodes draw rich, arbitrary viewer overlays: overlaid images (per-pixel data such as masks, drawn over the viewer image, never into renders), handles, points, lines, text and free-form drawing from native nodes, not just the stock Transform/CornerPin/Position/Rectangle interacts. Its first consumers are the darktable darkroom-only aids M80 - Native RAW Support deferred here: the highlights mask views (clipped, candidates, combine, strength), the dual-demosaic mask, the capture-sharpen sharpened-areas and radius views, and hot-pixel Mark Fixed (rawprepare/demosaic/highlights/hotpixels at darktable `release-5.6.2`).

Scouted facts (2026-10-23, re-check at elaboration): `EffectInstance::drawOverlay` (`Engine/EffectInstance.h:2552`) is an empty virtual no native node overrides; native nodes only call `Node::addRectangleInteract` (`Engine/NodeOverlay.cpp:317`); host primitives live in `Gui/HostOverlay.cpp` (points `:1172`, handles `:1246`, line loops `:1312`, text `:1036`); OFX overlays get GL via `Engine/OfxOverlayInteract.cpp`; no overlay path receives image data (viewer pixels go only to the viewer texture, `Engine/ViewerInstance.cpp:2037-2112`); the viewer-render flag exists as `isRenderUserInteraction` in `ParallelRenderArgs` (`Engine/ParallelRenderArgs.h:308`) but nodes can't query it.

Filed 2026-10-23 at the user's request.

Blocked on: M80 - Native RAW Support (its deferred raw aids are the first consumers and settle what an overlay image must carry), and the design of the overlay-image path — how a node publishes per-pixel overlay data beside, not into, its render — which should be settled with the user before tasks are written.

Acceptance sketch:
- A native node draws handles, points and text with no OFX.
- A node publishes a per-pixel overlay image that the viewer composites over its image without affecting any render or cache.
- The deferred darktable aids are added on top.
