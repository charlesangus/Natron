# ColorLookup

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| ColorLookup | transcendental | `kColorLookupTolerance` | |

ColorLookup holds `transcendental()` in the standard and film-like modes, including values outside
the table's fixed domain [0, 1], which the shader evaluates from the curve's keys with the key time and the cubic in two
floats (a key time between two floats, times a steep slope, would otherwise move the value far
beyond the bound), and blends the table in two floats to round as the CPU's double does. Weighted standard holds 5e-4 relative and luminance 3e-5: they
divide a curve value by a channel's distance to 0 or 1, or by the luminance, which grows a float
rounding as the divisor nears 0 (measured 2.1e-4 on RADV with "(Un)premult by", 1.3e-5 without, and
1.05e-5 for luminance at the 1e-8 floor). Weighted standard's triangle shifts a channel above a pixel channel at or over 1 by the move of that channel, where the OpenFX plug-in divided by zero or by a negative distance, and the shader does the same. Weighted standard also skips the colour of a pixel with a
denormal colour channel, whose quotient depends on the device's denormal arithmetic. Pixels of two
components stay on the CPU, as does a curve of more than 128 keys, and a curve `CurveSnapshot::packGpu`
cannot bound within 1e-6 of the CPU: so steep before its first key that the CPU's own double
rounding exceeds that, or with values and slopes so large that two floats do not hold them
(`Tests/Native/NativeColorLookupGpu_Test.cpp`).
