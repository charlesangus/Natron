# LensDistortion

Adds or removes lens distortion with one of eight models: Nuke, PFBarrel, 3DE Classic, 3DE4
Anamorphic Degree 6, 3DE4 Radial Fisheye Degree 8, 3DE4 Radial Standard Degree 4, 3DE4 Anamorphic
Standard Degree 4 and PanoTools. Only the selected model's parameters are shown. `direction`
Distort applies the distortion and Undistort removes it. For each output pixel the node solves the
opposite direction to find where in the Source it reads, so Distort evaluates the model's undistort
function and Undistort its distort function. A damped Newton solve with analytic Jacobians does the
inversion.

`outputMode` Image resamples the Source. STMap writes the map instead: red and green hold the
position read, normalized to the Source format ((0,0) is its bottom-left corner, (1,1) its top-right
one, as STMap expects), blue is 0, and alpha is 1 where the model has a solution and 0 elsewhere.
Feeding that map to STMap reproduces the Image output.

The model is defined over the reference format, which the extent knobs choose: by default the
Source format (its region of definition under `useRoD`, or when it has no format), otherwise a
format, a rectangle, or the project format. `overscan` makes the output format that much larger,
in percent of the reference width and height, split evenly between opposite sides, so that what
the distortion pushes out of the frame is kept. The region of definition is where the edges of the
Source land, sampled at 11 points per edge as openfx-misc does. With `cropToFormat`, the sides
where the Source stayed inside its format are cropped to the output format.

A pixel where the model has no solution (outside its domain, or where the solve does not converge)
is black, never NaN. **Strong Nuke distortion on an anamorphic format leaves black corners**: the
distort direction has no solution there. 3DE4 Radial Fisheye reports rays at or past 90 degrees from
the axis as having no solution, unlike ldpk, which clamps them.

Filtering uses the model's analytic Jacobian, so strong distortion is filtered by its real local
scale. openfx-misc used an identity Jacobian. The impulse filter, and draft renders, skip it and
take the nearest pixel.

Choosing a PFBarrel `.pfb` file sets C3, C5, Center and Squeeze from it. A file with several keys
sets keyframes, each with its own values. openfx-misc gave every key the first key's values and
left Squeeze alone. 3DE and PFBarrel honour a format whose origin is not (0,0).

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| LensDistortion (LensDistortion) | exact | `ParityTolerance::exact()` | Impulse filter against the Source sampled at `LensModels` coordinates; STMap output through STMap against Image output |
| LensDistortion (LensDistortion) | custom | `ParityTolerance::make(...)` | Distort then Undistort against the Source (cubic, interior only); half render scale against a downscaled full render |
