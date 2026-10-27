# Premult and Unpremult

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Premult | arithmetic | `ParityTolerance::arithmetic()` | a plain product, so normally exact |
| Unpremult | arithmetic | `ParityTolerance::arithmetic()` | a divisor at or below FLT_EPSILON leaves the value as it is |

- **Maths.** Premult: `v·d`. Unpremult: `v / d` where `d` is usable (above FLT_EPSILON),
  else `v`. `d` is the "(Un)premult by" channel and `v` each processed channel.
- **Divisor.** The "Premult by" / "Unpremult by" selector picks any layer's channel of the
  source, alpha by default. Picking None makes the node an identity.
- **The divisor channel itself.** When the Channels take in the divisor channel ("All", or
  the rgba layer with A on), it passes through: neither node scales it by itself, so a Premult
  never squares alpha and an Unpremult never sets it to 1, as in the host's own (un)premultiply.
- **Outside the divisor.** Where the divisor's image has no pixel the divisor reads 1, so the
  value is kept; the OpenFX plug-in read 0 there.
- **No premultiplication state.** The host carries no premultiplied/unpremultiplied flag on
  images, so these nodes set none.

The GPU reads the divisor from the processed plane, so a divisor in another layer renders on the
CPU (`Tests/Native/NativeColourUtilGpu_Test.cpp`).
