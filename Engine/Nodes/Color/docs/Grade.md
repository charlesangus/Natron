# Grade

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Grade | transcendental | `kGradeTolerance` | gamma |

- **Deliberate fix.** Grade, ColorCorrect and Saturation default "Clamp Black" off, so a default node is an identity and passes negatives and values above 1 through; the recorded cases turn it on, as OFX's default had it.

- **Deliberate fix.** Grade raises a gamma below 0.01 to 0.01, where OFX made a gamma at or below 0 a step at 1; no recorded case sets such a gamma.

Grade holds `gradefix::kUlpBound` (`Tests/Gpu/GradeFixture.h`), or a per-value bound that grows with |ln y| and 1 / gamma for the float rounding of the exponent and of A * input + B; its 0.01 gamma floor is applied on the host before the parameters are packed, so the device never sees a gamma at or below 0, and is what keeps 1 / gamma, and with it that bound, small. A subnormal input may come back as anything between the results for 0 and for the smallest normal float, as a device's `log2` may flush it or misread its exponent, which at gamma 0.01 is an order-one difference.
