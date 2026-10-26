# IDistort

Displaces the Source by a per-pixel offset read from the UV input. Output pixel (x, y) samples
the Source at `(x + 0.5 + (u - uvOffset.u) * uvScale.u, y + 0.5 + (v - uvOffset.v) * uvScale.v)`,
where u and v are the channels `channelU` and `channelV` select from any layer of the UV input
(by default rgba.R and rgba.G). A channel the UV input lacks, or a selector set to None, reads 0.
Offsets are in full-resolution pixels and are multiplied by the render scale.

Every filter except impulse sizes its footprint from the Jacobian of the sample point. The Jacobian
comes from central differences of the coordinates of neighbouring pixels. Those coordinates are
read one pixel past the render window, so the result does not depend on how the frame is tiled.
Impulse takes the nearest pixel and skips the Jacobian. A non-finite UV value makes that pixel
black. NaN never reaches the output.

The Source is read over its whole region of definition, and the UV input over the render window
grown by one pixel. The output keeps the Source's region of definition. With the UV input
disconnected, both selectors None, or a zero `uvScale`, the node is an identity of the Source.
The host mask and mix apply as for every staged node.

The optional Map input (input 3, after the Mask) scales the displacement per pixel: output pixel
(x, y) samples the Source at `(x + 0.5 + du * m, y + 0.5 + dv * m)`, du and dv being the offsets
above and m the Map value. Map Channel picks one channel of any of the Map's layers (alpha by
default) and Invert Map inverts it; the value is clamped to [0, 1] and remapped by Map Gamma and
Map Response as in IBlur (`Engine/Nodes/Filter/docs/IBlur.md`). Where m is 0 the pixel samples its
own centre exactly, whatever the UV holds, so a black Map is an identity of the Source even where
the UV is not finite; where m is 1 the displacement is the unmapped one. With no Map connected the
whole displacement applies and the output is bit for bit the node's without the input.

- The Map is sampled in output space, at the pixel being written, as the UV is, not where that
  pixel comes from in the Source. Outside its image the Map value is 0, neither inverted nor remapped (no
  displacement).
- The Jacobian takes the Map's gradient with the UV's, from the same neighbouring coordinates, so
  the Map is read one pixel past the render window. A hard Map edge between black and white tears
  the image along it when the displacement is large; soften the Map to bend the image instead.
- IDistort is a per-pixel warp: it never concatenates with transforms above or below it, and each
  of those resamples the image on its own.
- The GPU reads the Map as an extra input and remaps it with `mapremap.slang`, the CPU's remap
  operation for operation.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| IDistort (IDistort) | exact | `ParityTolerance::exact()` | Impulse against shifted Source and against `Resampler::interpolate()` at each sample point; mip 1 against the downscaled full-resolution render |
| IDistort (IDistort) | filtered | `1e-5` absolute | A constant UV with the cubic filter against the Source sampled at the same offset |
| IDistort (IDistort) | exact | `ParityTolerance::exact()` | Map: a white Map against no Map, a Map of 0.5 against UV Scale 0.5, a black Map (impulse) against the Source, a per-pixel Map (impulse) against `Resampler::interpolate()` (`NativeIDistortAmount_Test.cpp`) |
| IDistort (IDistort) | resampling | per filter, as without the Map | the GPU against the CPU with a Map, inverted, gamma 2 with Smooth, and from another channel (`NativeWarpGpu_Test.cpp`) |
