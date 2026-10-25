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

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| IDistort (IDistort) | exact | `ParityTolerance::exact()` | Impulse against shifted Source and against `Resampler::interpolate()` at each sample point; mip 1 against the downscaled full-resolution render |
| IDistort (IDistort) | filtered | `1e-5` absolute | A constant UV with the cubic filter against the Source sampled at the same offset |
