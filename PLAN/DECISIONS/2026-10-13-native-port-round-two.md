# Native port round two

User decisions, 2026-10-13, planning the next round of OFX → native node ports (`/cat-plan --grill`):

- **Families ported:** colour utilities (M122), shape generators (M128), a clean-sheet Noise (M129), routing and mix (M121), time remaps (M124) and multi-frame nodes (M125), warps (M113) and lens distortion (M132), sharpen/glow (M123), edge-preserving and directional filters (M130), a new defocus family (M131, follow-up M133), and the OCIO nodes (M127, GPU on M126 - Runtime GPU Shaders).
- **Depth:** redesign and consolidate. Nodes get Nuke-like knob sets and scene-linear maths, not OFX parity (extends `2026-10-12-native-nodes-drop-ofx-legacy`). Siblings fold where one node covers them (the 18 colour converters → `Colorspace`; ColorSuppress → `Despill`; CornerPinMasked → `CornerPin`; OCIOLogConvert → an `OCIOColorSpace` preset).
- **GPU:** each port milestone ships its GPU kernels and parity tests itself, after the GPU milestones it builds on. Time nodes all go on the GPU, to keep regions resident.
- **Order:** none. The milestones declare `Deps` only, and PMs run any eligible one in parallel. M120 - Native Node Port Kit runs first and removes the shared manifests (registration, kernel lists, cost seeds, icons, docs, OFX retirement) that would otherwise serialise them by file overlap.
- **Retired without a native replacement:** Log2Lin, PLogLin (OCIO covers them), HueCorrect1, TimeDissolve (animate Dissolve's `which`), TimeBufferRead/Write, Rand, CImg Noise and Plasma (the new Noise covers them), GodRays (a DirBlur mode).
- **Stay on OFX for now:** ColorBars, NoOp, CopyRectangle, the ImageMagick arena nodes, DenoiseSharpen, Inpaint, ContactSheet, SeExpr, Shadertoy, Card3D, Deinterlace, ChromaBlur, ErodeSmooth, SharpenShock/InvDiff, Smooth, Denoise, Equalize/HistEQ.
- **Noise source:** a spike chooses between porting Blender Cycles' kernels (Apache-2.0, so binaries are effectively GPLv3) and OpenSimplex2 (CC0) plus our own cellular noise. 4D noise is required.
