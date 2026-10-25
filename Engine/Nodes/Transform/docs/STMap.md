# STMap

Moves pixels around the Source using a UV map. For each output pixel, the UV input's channels
`channelU` and `channelV` give a normalized position in the Source format. Each knob names a
layer and a channel, from any layer of the input, the same way as IDistort.
(0,0) is the bottom-left corner of the format and (1,1) its top-right corner. That position is first offset and scaled
(`(u - uvOffset) * uvScale`), then wrapped per axis (`wrapU`/`wrapV`: Clamp, Repeat, Mirror) into
the Source format, which falls back to the Source region of definition when the Source has no
format. A pixel centre maps to itself when U = (x + 0.5) / width.

The output format is the UV input's. `formatFrom` chooses whether the output covers the UV format
or the UV region of definition. A UV channel that the input lacks, or None, reads 0, and so does UV outside
its image. A non-finite coordinate reads black, except under Clamp wrap, where an infinity clamps
to the edge. With UV or Source disconnected, the output is black.

Filtering uses the Jacobian of the map, taken from central differences of UV between neighbouring
pixels. The rows are one pixel wider than the window so that the result does not depend on where
a tile ends. The impulse filter skips the Jacobian and takes the nearest pixel. There is no alpha
coverage multiply and no UV unpremultiply.

The optional Map input (input 3, after the Mask) scales how far each pixel moves: with c the
centre of output pixel (x, y) and p the Source position its UV gives, the pixel samples
`c + (p - c) * m`, m being the Map value. Map Channel picks one channel of any of the Map's layers
(alpha by default) and Invert Map inverts it; the value is clamped to [0, 1] and remapped by Map
Gamma and Map Response as in IBlur (`Engine/Nodes/Filter/docs/IBlur.md`). Where m is 0 the pixel
samples its own centre exactly, so a black Map is an identity of the Source, non-finite UV
included; where m is 1 the pixel samples p as without a Map; at 0.5 it goes half-way to its UV
position. UV Alpha blacks a pixel out only where m is above 0: a pixel the Map keeps in place
shows the Source whatever the UV's alpha. With no Map connected the output is bit for bit the
node's without the input.

- The Map is sampled in output space, at the pixel being written, as the UV is. Outside its image
  the raw Map value is 0 (the pixel stays in place).
- The Jacobian takes the Map's gradient with the UV's, so the Map is read one pixel past the
  render window. A hard Map edge between black and white tears the image along it when the UV
  moves pixels far; soften the Map to bend the image instead.
- STMap is a per-pixel warp: it never concatenates with transforms above or below it.
- The GPU reads the Map as an extra input and remaps it with `mapremap.slang`, the CPU's remap
  operation for operation.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| STMap (STMap) | exact | `ParityTolerance::exact()` | Impulse filter: identity map, wrap modes, offset/scale against index arithmetic |
| STMap (STMap) | resampling | `ParityTolerance::resampling()` | Filtered (cubic) identity map against the Source |
| STMap (STMap) | exact | `ParityTolerance::exact()` | Map: a white Map against no Map, a black Map (impulse, UV Alpha on) against the Source, a Map of 0.5 and a per-pixel Map (impulse) against `Resampler::interpolate()` at `c + (p - c) * m` (`NativeIDistortAmount_Test.cpp`) |
| STMap (STMap) | resampling | per filter, as without the Map | the GPU against the CPU with a Map, its remap knobs and UV Alpha (`NativeWarpGpu_Test.cpp`) |
