# M268 - Edge-Aware Region Independence

> Stub — elaborate before starting (PLAN-FORMAT.md §5).

Filed 2026-10-28 from the audit after M215 - Colour Equalizer's guided filter produced output that depended on the requested region (subsampling grid anchored at the buffer corner). Every other EdgeAware user renders over the whole source frame, so it is correct today, but nothing enforces it.

- Library contract: `GuidedFilter.h`, `Eigf.h`, `Wavelets.h`, `BoxFilter.h` take an explicit frame origin and full size (like `ColorEqualizerFrame`), so a windowed caller cannot silently change the grid; or assert the buffer is the full RoD.
- Regression test pattern for every EdgeAware/darktable node (M210 DenoiseProfiled, M211 Retouch, M212 ToneEqualizer, M213 ContrastTexture, M215 ColorEqualizer, RawHighlights): full frame vs several offset sub-windows and tiles, bit-identical (or within ulps).
- Clamp whole-frame buffers to the format when the source RoD is infinite (M212 ToneEqualizer, M210 DenoiseProfiled don't; M211/M213 do).
- M213 ContrastTexture recomputes its full-frame map on every tile — cache it by source hash/scale/RoD (cost).
- M210 Compute Variance readout covers "the last region rendered": measure over the whole frame or label it as viewer-region.

Deps: M209, M210, M211, M212, M213, M215.
- M215 ColorEqualizer now processes the whole RoD per window (CPU cached; GPU recomputes per window): add a frame-size guard for the GPU path like ToneEqualizer's UINT32 check in canRenderOpOnGpu, and consider a device-side cache.
