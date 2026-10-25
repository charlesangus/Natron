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

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| STMap (STMap) | exact | `ParityTolerance::exact()` | Impulse filter: identity map, wrap modes, offset/scale against index arithmetic |
| STMap (STMap) | resampling | `ParityTolerance::resampling()` | Filtered (cubic) identity map against the Source |
