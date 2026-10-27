# Despill

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Despill | arithmetic, plus an absolute bound | `expectNear()`, 4e-6 times the largest channel | the CPU is held to a double-precision reference of the same maths; the rotation sums three single-precision products, so the bound follows the channels, not the result |
| Despill, GPU | arithmetic | `ParityTolerance::arithmetic()` | the rotation, limit and move run statement for statement on both sides with the host's baked matrix |

A clean-sheet despill that also does what OpenFX's Despill and ColorSuppress did
(`Tests/Native/NativeDespill_Test.cpp`). It does not port either plug-in's controls.

Maths, for a source colour `p` and a Screen Color `S` with chroma `max(S) - min(S)`:

- **Spill axis.** `R` rotates RGB about the grey axis by the angle that takes the screen's hue
  to green's, so `(r', g', b') = R·p`. For a green screen `R` is the identity; for a blue one
  `r'` and `b'` are green and red.
- **Limit.** `L = limit(r', b')`: their average (default), the larger (Max), the smaller (Min)
  or `w0·r' + w1·b'` (Custom, Limit Weights).
- **Spill map.** `m = max(g' - (1 - expand)·L, 0) / k`, where `k` is the same expression for the
  screen's chroma direction `D = (S - min(S)) / chroma`, so the screen colour itself maps to 1.
- **Move.** `p += strength·m·(C - lum(C) + restore·lum(D) - D)`, with `C` the Replace Color and
  `lum` the working space's normalised luma weights. A pixel of the screen's hue drops to its
  limit; Replace Color adds only its chroma, and Luma Restore at 1 keeps the luminance.
- **Spill Map output** (None or Alpha) writes `m`, before Strength, to alpha when alpha is
  processed; the output is then RGBA. A staged node cannot write another layer.

- **Grey screen colour** (no spill axis) or Strength 0: an identity unless Spill Map is Alpha.
- **ColorSuppress.** A magenta, cyan, yellow, red or blue screen colour with the Max limit
  suppresses that colour as ColorSuppress did.
- **Expand** scales the limit by `1 - expand`, as OpenFX's Despill did.
- **No clamps.** HDR and negative values are neither clipped nor treated as spill when `m` is
  negative or NaN.

The host bakes the rotation and the move vector; the kernel reads them and follows the CPU
statement for statement with `precise` arithmetic (`Tests/Native/NativeDespillGpu_Test.cpp`).
