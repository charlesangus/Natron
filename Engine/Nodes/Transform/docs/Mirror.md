# Mirror

Reverses the rows (`flip`) and the columns (`flop`) of the image about the centre of its region
of definition. The reflection is taken on the pixel grid at the render scale, so a region of odd
size or away from the origin reverses onto itself pixel for pixel.

Mirror concatenates: it hands the host its matrix, so a chain of Mirror and Transform nodes is
resampled once, by the last node of the chain that renders, with that node's filter. Rendered
alone, or with only unfiltered neighbours, it samples with the impulse filter at pixel centres and
is an exact index reversal. A Mirror that renders after an upstream concatenated transform applies
its own impulse filter to the combined matrix.

Tolerance:

| Node (ID) | Class | Test constant | Notes |
|---|---|---|---|
| Mirror (Mirror) | exact | `ParityTolerance::exact()` | CPU against the index reversal, and GPU against CPU |
