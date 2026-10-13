# M133 - ZDefocus Lens Aberrations

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A follow-up to M131 - Native Defocus Family that makes ZDefocus and Defocus behave like real lenses:
- **Cat-eye (optical) vignetting.** The bokeh is clipped by a second aperture as a function of image position, so discs turn into cat-eyes towards the frame edges.
- **Chromatic aberration.** The CoC or kernel scale differs per channel, giving coloured bokeh fringes.
- **Anamorphic squeeze.** Oval bokeh with a squeeze ratio.
- **Possibly textured kernels.** Swirly bokeh and onion-ring kernel textures.

Each ships CPU and GPU, with parity tests, on M131 - Native Defocus Family's bokeh kernel library and ZDefocus gather.

Deps: M131 - Native Defocus Family.

Blocked on: M131 - Native Defocus Family merged (kernel library and ZDefocus gather design).

Acceptance sketch:
- A point light near the frame edge renders a cat-eye bokeh. The same light at the centre renders a full disc.
- Chromatic aberration produces per-channel bokeh sizes. With it off, the output equals M131 - Native Defocus Family's.
- An anamorphic squeeze of 2 renders oval bokeh with a 2:1 aspect.
- CPU and GPU match within M131 - Native Defocus Family's stated ZDefocus tolerance.

## Decisions

- 2026-10-13 — **Created as a follow-up (user, /cat-plan grill):** an improved ZDefocus with aberrations, kept out of M131 - Native Defocus Family's v1.
