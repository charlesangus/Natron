# Blur

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Blur | IIR | `kBlurTolerance` | box filters share the class |

- **Deliberate fix.** Blur, EdgeDetect and Erode/Dilate filter the whole window plus the halo whatever the mask, where CImg shrinks it past mask-zero rows and columns, which moves the IIR filters' lines. Blur's `mask-mix` case trims only rows and columns within the halo of the frame's edge, so its buffer, and its reference, are unchanged; EdgeDetect's is replaced by a check of the mask blend against the node's own unmasked render.

- **Deliberate fix.** Blur has no derivative orders (EdgeDetect computes derivatives), no border-condition choice (it is black outside the image), no alpha threshold, no "Expand RoD" (the region of definition always grows by the filter's reach) and no "Uniform" toggle (the size knob folds its dimensions) and no "(Un)premult by" (a premult-blur-unpremult chain uses the Premult and Unpremult nodes); the OFX plug-in's defaults are those behaviours, so the recorded cases are unchanged.

Blur's FIR Gaussian holds
2e-6 absolute. Blur's quasi-Gaussian, Gaussian (IIR), box, triangle and quadratic filters hold `iir()`: one thread runs
each line through the CPU's recursion with its state in two floats where the CPU uses double (plain float drifts past 1e-3
for the IIR Gaussian at large sizes), and filters the same rect as render(), the window grown by the
halo and clipped to the region of definition. Blur's knobs are size, filter, "Crop To Format" and the standard channel, mask and mix
knobs, so every setting of it is eligible for the GPU unless the FIR Gaussian's radius exceeds the kernel's tile
(`Tests/Native/NativeBlurGpu_Test.cpp`).
