# Defocus harness results

This report scores today's ZDefocus against a test-only port of Niik-l's scatter engine (`DefocusCore`) on six analytic scenes, and recommends a direction for M294 - ZDefocus Engine Revisit. The harness is `Tests/Native/DefocusHarness_Test.cpp` with its sources in `Tests/Native/DefocusReference/`.

## How it was run

- Each scene is rendered at 512×288 twice by the in-tree thin-lens renderer: a pinhole pass (RGBA, planar depth, world position and normal) that both engines take as input, and a Monte Carlo pass over the lens aperture (256 stratified samples per pixel, seed 1) that is the truth.
- ZDefocus runs through the node at 8, 16 and 32 layers, with its Lens mode set to the scene's camera so its circle of confusion matches the truth.
- Niik-l's engine runs with every combination of edge mode (Unmix, Snap, Off), fill behind the foreground (on, off) and position/normal input (P/N, on, off), all with occlusion edges set to Split. One extra run, Unmix with fill and occlusion edges set to Fast, shows what the forced slice boundaries cost or gain.
- Metrics, against the truth, over premultiplied RGBA:
  - **RMSE** over the whole frame.
  - **Edge RMSE** over the edge band: pixels within the larger circle of confusion of any depth discontinuity in the pinhole depth. This is the main score.
  - **Halo**: colour the candidate adds where the truth is uncovered. **Hole**: colour it loses where the truth is covered. Both are averaged per pixel of the frame.
  - **Seconds**: wall time of one CPU render. ZDefocus's figure includes the node graph's overhead, Niik-l's is a bare function call, and each is a single run, so treat them as orders of magnitude, not a benchmark.
- The truth carries Monte Carlo noise that every candidate pays equally. Differences in the fourth significant digit are within that noise.

The crops in `harness/` are 256×256 half-float EXRs centred on each scene's edge band. Files ending in `-diff` hold candidate minus truth.

## Summary

Lowest edge RMSE per engine per scene; the last column is Niik-l's best over ZDefocus's best.

| Scene | Best ZDefocus | Edge RMSE | Best Niik-l | Edge RMSE | Niik-l / ZDefocus |
|---|---|---:|---|---:|---:|
| sharpFgBlurBg | layers8 | 0.055198 | off-fill-split | 0.054942 | 1.00 |
| blurFgSharpBg | layers8 | 0.060859 | unmix-fill-split | 0.027563 | 0.45 |
| aliasedEdge | layers8 | 0.03526 | snap-fill-split | 0.02365 | 0.67 |
| grazingFloor | layers8 | 0.020349 | unmix-fill-fast | 0.024732 | 1.22 |
| highlightsBehindEdge | layers32 | 0.11227 | off-fill-split | 0.10779 | 0.96 |
| eclipsedBokeh | layers32 | 0.064328 | unmix-fill-split | 0.05536 | 0.86 |

## Where each engine wins

- **blurFgSharpBg** (blurred foreground over a sharp background): Niik-l by the widest margin, 0.027563 against 0.060859. ZDefocus keeps the foreground's colour right up to its edge, so the background that the lens sees past the blurred foreground never appears; its hole energy is 0.011349 against Niik-l's 0.0040303. Compare `harness/blurFgSharpBg-truth.exr`, `harness/blurFgSharpBg-zdefocus-layers16.exr` and `harness/blurFgSharpBg-niikl-unmix-fill-split.exr`.
- **aliasedEdge** (a pixel-coverage mix of two depths): Niik-l with an edge split, 0.02365 (Snap) and 0.024011 (Unmix) against 0.03526. Without the split (Off, fill on) Niik-l is worse than ZDefocus at 0.049354, and its error runs along the slanted edge: `harness/aliasedEdge-niikl-off-fill-split-diff.exr` against `harness/aliasedEdge-niikl-unmix-fill-split-diff.exr`.
- **eclipsedBokeh**: Niik-l, 0.05536 against 0.064328. ZDefocus has far more halo, 0.00025608 at 32 layers against 3.178e-05, and its difference crops show why: it spreads the foreground's blurred edge further over the dark wall than the truth does.
- **highlightsBehindEdge**: Niik-l narrowly, 0.10779 against 0.11227 in the edge band and 0.047616 against 0.052663 over the frame.
- **sharpFgBlurBg**: effectively a tie, 0.054942 against 0.055198.
- **grazingFloor**: ZDefocus at 8 layers, 0.020349. Niik-l's best is 0.024732 with Fast occlusion edges, and 0.065936 with the default Split, whose forced slice boundaries cut the floor into visible bands: `harness/grazingFloor-niikl-unmix-fill-split-diff.exr` against `harness/grazingFloor-zdefocus-layers8-diff.exr`.

ZDefocus does not get better with more layers. 8 layers is its best in four scenes. On grazingFloor its edge RMSE goes from 0.020349 at 8 layers to 0.031808 at 16 and 0.027623 at 32, and on aliasedEdge from 0.03526 to 0.041524 and 0.043135, with halo growing from 0.00031912 to 0.0007368. Only the emissive scenes gain from more layers. There, 8 layers quantises disc sizes, so eclipsedBokeh's discs come out small and ringed (edge RMSE 0.078657 at 8 layers against 0.064884 at 16; `harness/eclipsedBokeh-zdefocus-layers8.exr`). An Auto layer count needs to know what makes the error grow with layer count before it can pick one.

## Bitten discs in eclipsedBokeh

Neither engine reproduces the truth's bitten discs, and on this input neither can. In `harness/eclipsedBokeh-truth.exr` the bites are the thin crescents inside the foreground's blurred edge. They come from points that the pinhole view hides behind the foreground: part of the lens sees around the edge, so a sliver of each disc shows. Those points are not in either engine's input, and fill extends the wall's colour, not light that it never saw. Every candidate leaves the crescents out; `harness/eclipsedBokeh-niikl-unmix-fill-split.exr` matches ZDefocus at 16 and 32 layers there.

The discs that the pinhole does see sit clear of the foreground's blur in this framing, so the scene does not yet test whether a visible disc is cut along the foreground's edge. Both engines draw those discs at the truth's size (ZDefocus from 16 layers up) and slightly dimmer, alike for every candidate, so that loss comes from the pinhole input. highlightsBehindEdge shows the same thing at a sharp edge: the truth has an extra, partly hidden column of discs that no candidate draws.

## Position and normal

P/N makes no difference in 34 of the 36 pairs, to the five significant digits the harness records. On grazingFloor, Unmix with fill gets slightly worse with P/N and Unmix without fill moves in the last digit. The engine consults P/N only where the depth step alone is ambiguous, and the analytic depth here is clean, so depth decides every edge. These scenes do not cover noisy or filtered depth (volumes, fur, motion blur), where P/N might matter. On this evidence P/N does not help, even on the grazing floor.

Edge RMSE without and with P/N for each Niik-l configuration:

| Scene | Config | Edge RMSE without P/N | With P/N | Difference |
|---|---|---:|---:|---:|
| sharpFgBlurBg | unmix-fill-split | 0.055212 | 0.055212 | 0 |
| sharpFgBlurBg | unmix-nofill-split | 0.083414 | 0.083414 | 0 |
| sharpFgBlurBg | snap-fill-split | 0.055258 | 0.055258 | 0 |
| sharpFgBlurBg | snap-nofill-split | 0.08336 | 0.08336 | 0 |
| sharpFgBlurBg | off-fill-split | 0.054942 | 0.054942 | 0 |
| sharpFgBlurBg | off-nofill-split | 0.083359 | 0.083359 | 0 |
| blurFgSharpBg | unmix-fill-split | 0.027563 | 0.027563 | 0 |
| blurFgSharpBg | unmix-nofill-split | 0.12444 | 0.12444 | 0 |
| blurFgSharpBg | snap-fill-split | 0.032236 | 0.032236 | 0 |
| blurFgSharpBg | snap-nofill-split | 0.12739 | 0.12739 | 0 |
| blurFgSharpBg | off-fill-split | 0.032236 | 0.032236 | 0 |
| blurFgSharpBg | off-nofill-split | 0.12739 | 0.12739 | 0 |
| aliasedEdge | unmix-fill-split | 0.024011 | 0.024011 | 0 |
| aliasedEdge | unmix-nofill-split | 0.057543 | 0.057543 | 0 |
| aliasedEdge | snap-fill-split | 0.02365 | 0.02365 | 0 |
| aliasedEdge | snap-nofill-split | 0.058004 | 0.058004 | 0 |
| aliasedEdge | off-fill-split | 0.049354 | 0.049354 | 0 |
| aliasedEdge | off-nofill-split | 0.066243 | 0.066243 | 0 |
| grazingFloor | unmix-fill-split | 0.065936 | 0.066454 | +0.000518 |
| grazingFloor | unmix-nofill-split | 0.13405 | 0.13404 | -1e-05 |
| grazingFloor | snap-fill-split | 0.072126 | 0.072126 | 0 |
| grazingFloor | snap-nofill-split | 0.13479 | 0.13479 | 0 |
| grazingFloor | off-fill-split | 0.071299 | 0.071299 | 0 |
| grazingFloor | off-nofill-split | 0.135 | 0.135 | 0 |
| highlightsBehindEdge | unmix-fill-split | 0.10802 | 0.10802 | 0 |
| highlightsBehindEdge | unmix-nofill-split | 0.12082 | 0.12082 | 0 |
| highlightsBehindEdge | snap-fill-split | 0.10808 | 0.10808 | 0 |
| highlightsBehindEdge | snap-nofill-split | 0.12149 | 0.12149 | 0 |
| highlightsBehindEdge | off-fill-split | 0.10779 | 0.10779 | 0 |
| highlightsBehindEdge | off-nofill-split | 0.12149 | 0.12149 | 0 |
| eclipsedBokeh | unmix-fill-split | 0.05536 | 0.05536 | 0 |
| eclipsedBokeh | unmix-nofill-split | 0.075587 | 0.075587 | 0 |
| eclipsedBokeh | snap-fill-split | 0.056346 | 0.056346 | 0 |
| eclipsedBokeh | snap-nofill-split | 0.075482 | 0.075482 | 0 |
| eclipsedBokeh | off-fill-split | 0.056287 | 0.056287 | 0 |
| eclipsedBokeh | off-nofill-split | 0.075477 | 0.075477 | 0 |

## Which ideas account for the gap

Edge RMSE as Niik-l's ideas are added one at a time, against ZDefocus's best:

| Scene | Best ZDefocus | Scatter only (`off-nofill-split`) | + fill (`off-fill-split`) | + Unmix edge split (`unmix-fill-split`) | Unmix, fill, Fast edges (`unmix-fill-fast`) |
|---|---:|---:|---:|---:|---:|
| sharpFgBlurBg | 0.055198 | 0.083359 | 0.054942 | 0.055212 | 0.055212 |
| blurFgSharpBg | 0.060859 | 0.12739 | 0.032236 | 0.027563 | 0.027563 |
| aliasedEdge | 0.03526 | 0.066243 | 0.049354 | 0.024011 | 0.024011 |
| grazingFloor | 0.020349 | 0.135 | 0.071299 | 0.065936 | 0.024732 |
| highlightsBehindEdge | 0.11227 | 0.12149 | 0.10779 | 0.10802 | 0.10802 |
| eclipsedBokeh | 0.064328 | 0.075477 | 0.056287 | 0.05536 | 0.05536 |

- **The scatter itself** accounts for none of the gain. On its own (no edge split, no fill) it is worse than ZDefocus in every scene, from 0.075477 against 0.064328 on eclipsedBokeh to 0.135 against 0.020349 on grazingFloor. Without fill it leaves a hole behind every foreground; grazingFloor's hole energy reaches 0.16181.
- **Fill** is the largest single gain. It improves every scene and takes blurFgSharpBg from 0.12739 to 0.032236, about half of ZDefocus's 0.060859.
- **Edge split** gains on edges that mix two depths: aliasedEdge goes from 0.049354 to 0.024011 with Unmix, and blurFgSharpBg from 0.032236 to 0.027563. It is neutral elsewhere. Unmix and Snap are level overall: Unmix is better on blurFgSharpBg, Snap by a hair on aliasedEdge.
- **Forced slice boundaries** (Split against Fast) give identical scores in five scenes. On grazingFloor, Split is much worse (0.065936 against 0.024732) and takes 0.94489 s against 0.47521 s.

## GPU residency

Both engines run on the CPU in this harness. ZDefocus already has a GPU path. For each idea, as part of a fully GPU-resident engine:

- **Edge split:** a per-pixel classification over a small neighbourhood, then each edge pixel becomes a front and a back sample with complementary weights. It is embarrassingly parallel: one compute pass, and in a layered engine an edge pixel writes into two layers. Cheap and feasible.
- **Fill:** an alpha-weighted distance-transform fill of each layer behind the nearer layers. Jump flooding computes the distance transform in log2(size) passes, and the weighting is a local gather. The cost scales with the number of layers that have holes; Niik-l limits it to the holes' bounding box. Feasible, and the largest new GPU cost.
- **Forced slice boundaries:** a global step. It histograms edge-pair intervals over fine depth bins, then places boundaries by greedy interval stabbing over that small array. Feasible as an atomic histogram plus a single-workgroup kernel, with no readback. Not worth it on this evidence.
- **Scatter:** the row-span scatter adds a sample's colour at the start of each disc row and subtracts it at the end, then takes a prefix sum along each row. On the GPU this needs float atomics, so the summation order is not deterministic and CPU/GPU parity becomes a tolerance rather than an exact match. The cost is samples times disc rows. Feasible, but it means replacing the engine, and the harness shows no quality gain from the scatter alone.
- **P/N:** extra inputs only, trivially feasible, but no measured gain.

## Recommendation for M294 - ZDefocus Engine Revisit

1. **Keep ZDefocus's layered, GPU-resident engine. Do not replace it with a scatter.** The scatter alone loses in every scene. The gains come from ideas that fit into layers.
2. **Port the fill:** an alpha-weighted distance-transform fill of each layer behind the nearer layers, done with jump flooding on the GPU. It should bring blurFgSharpBg's edge RMSE from 0.060859 towards 0.032236 and its hole energy from 0.011349 towards 0.0043803. Inside the scatter, fill improved every scene. Cost: a few full-frame passes per layer that has holes.
3. **Port the edge split** (Unmix, with Snap as a switch): an edge pixel splits between its front and back layers. It should bring aliasedEdge from 0.03526 towards 0.024011, at the cost of one extra pass and a second sample at edge pixels.
4. **Do not port** forced slice boundaries (no gain here, a large regression on grazingFloor, and twice the time) or P/N (no measured effect).
5. **Find out why ZDefocus's error grows with layer count** (grazingFloor, aliasedEdge, sharpFgBlurBg) before designing Auto layers. 8 layers is too few for highlights, and 16 or 32 layers cost edge quality elsewhere.

Uncertain:

- Fill and edge split were measured inside the scatter engine. Whether a layered gather gets the same gains is not proven. M294's first task should prototype both in ZDefocus's CPU path and re-run this harness before any GPU work. If the gather falls well short, the fallback is a GPU scatter with fill, edge split and Fast occlusion edges, which costs a new engine and an exact-parity guarantee.
- M294's eclipsed-bokeh criterion cannot be checked on today's eclipsedBokeh scene, because its bites come from hidden points (see above). Either reframe the scene so that discs visible in the pinhole fall under the foreground's blur, which M294 can be held to, or accept that bites from hidden points need data from behind the foreground (a clean-plate input, or deep input in M295 - Native DeepDefocus). This needs the user's decision.
- M294's acceptance asks for a better edge RMSE in every scene. On grazingFloor no Niik-l configuration beats ZDefocus at 8 layers, and sharpFgBlurBg is a tie, so for those two the bound should probably be "no worse".
- P/N was tested only on clean analytic depth.

## Per-scene results

The ZDefocus rows leave the edge-mode, fill, occlusion-edge and P/N columns blank because those are Niik-l settings.

### sharpFgBlurBg

Edge band: 25595 pixels. Bold: lowest edge RMSE in the scene. Italic: lowest ZDefocus edge RMSE.

| Engine | Layers / edge mode | Fill | Occlusion edges | P/N | RMSE | Edge RMSE | Halo | Hole | Seconds |
|---|---|---|---|---|---:|---:|---:|---:|---:|
| ZDefocus | 8 layers |  |  |  | 0.023099 | *0.055198* | 7.417e-05 | 0.0036404 | 0.12551 |
| ZDefocus | 16 layers |  |  |  | 0.023234 | 0.055545 | 0.00012445 | 0.0036948 | 0.19581 |
| ZDefocus | 32 layers |  |  |  | 0.023934 | 0.057233 | 0.00068139 | 0.0034922 | 0.21986 |
| Niik-l | Unmix | on | Split | off | 0.023091 | 0.055212 | 9.2931e-05 | 0.0029812 | 0.13777 |
| Niik-l | Unmix | on | Split | on | 0.023091 | 0.055212 | 9.2931e-05 | 0.0029812 | 0.13872 |
| Niik-l | Unmix | off | Split | off | 0.034811 | 0.083414 | 4.684e-05 | 0.011231 | 0.11725 |
| Niik-l | Unmix | off | Split | on | 0.034811 | 0.083414 | 4.684e-05 | 0.011231 | 0.12881 |
| Niik-l | Snap | on | Split | off | 0.02311 | 0.055258 | 9.6952e-05 | 0.0030151 | 0.13043 |
| Niik-l | Snap | on | Split | on | 0.02311 | 0.055258 | 9.6952e-05 | 0.0030151 | 0.13879 |
| Niik-l | Snap | off | Split | off | 0.034788 | 0.08336 | 4.6889e-05 | 0.011217 | 0.12536 |
| Niik-l | Snap | off | Split | on | 0.034788 | 0.08336 | 4.6889e-05 | 0.011217 | 0.15171 |
| Niik-l | Off | on | Split | off | 0.022978 | **0.054942** | 4.6698e-05 | 0.0030304 | 0.16645 |
| Niik-l | Off | on | Split | on | 0.022978 | **0.054942** | 4.6698e-05 | 0.0030304 | 0.18116 |
| Niik-l | Off | off | Split | off | 0.034788 | 0.083359 | 4.6531e-05 | 0.011217 | 0.16273 |
| Niik-l | Off | off | Split | on | 0.034788 | 0.083359 | 4.6531e-05 | 0.011217 | 0.185 |
| Niik-l | Unmix | on | Fast | off | 0.023091 | 0.055212 | 9.2931e-05 | 0.0029812 | 0.20895 |

### blurFgSharpBg

Edge band: 26554 pixels. Bold: lowest edge RMSE in the scene. Italic: lowest ZDefocus edge RMSE.

| Engine | Layers / edge mode | Fill | Occlusion edges | P/N | RMSE | Edge RMSE | Halo | Hole | Seconds |
|---|---|---|---|---|---:|---:|---:|---:|---:|
| ZDefocus | 8 layers |  |  |  | 0.026586 | *0.060859* | 0.00035733 | 0.011349 | 0.76293 |
| ZDefocus | 16 layers |  |  |  | 0.026597 | 0.060881 | 0.00034869 | 0.011403 | 0.69162 |
| ZDefocus | 32 layers |  |  |  | 0.026649 | 0.061 | 0.00034647 | 0.011481 | 0.57222 |
| Niik-l | Unmix | on | Split | off | 0.013294 | **0.027563** | 8.0096e-05 | 0.0040303 | 0.35087 |
| Niik-l | Unmix | on | Split | on | 0.013294 | **0.027563** | 8.0096e-05 | 0.0040303 | 0.36016 |
| Niik-l | Unmix | off | Split | off | 0.053183 | 0.12444 | 8.0096e-05 | 0.027117 | 0.31085 |
| Niik-l | Unmix | off | Split | on | 0.053183 | 0.12444 | 8.0096e-05 | 0.027117 | 0.32783 |
| Niik-l | Snap | on | Split | off | 0.015068 | 0.032236 | 0.00021934 | 0.0043803 | 0.34908 |
| Niik-l | Snap | on | Split | on | 0.015068 | 0.032236 | 0.00021934 | 0.0043803 | 0.43182 |
| Niik-l | Snap | off | Split | off | 0.054427 | 0.12739 | 0.00021934 | 0.02738 | 0.36593 |
| Niik-l | Snap | off | Split | on | 0.054427 | 0.12739 | 0.00021934 | 0.02738 | 0.33745 |
| Niik-l | Off | on | Split | off | 0.015068 | 0.032236 | 0.00021934 | 0.0043803 | 0.33867 |
| Niik-l | Off | on | Split | on | 0.015068 | 0.032236 | 0.00021934 | 0.0043803 | 0.36881 |
| Niik-l | Off | off | Split | off | 0.054427 | 0.12739 | 0.00021934 | 0.02738 | 0.32806 |
| Niik-l | Off | off | Split | on | 0.054427 | 0.12739 | 0.00021934 | 0.02738 | 0.35158 |
| Niik-l | Unmix | on | Fast | off | 0.013294 | **0.027563** | 8.0096e-05 | 0.0040303 | 0.3216 |

### aliasedEdge

Edge band: 8074 pixels. Bold: lowest edge RMSE in the scene. Italic: lowest ZDefocus edge RMSE.

| Engine | Layers / edge mode | Fill | Occlusion edges | P/N | RMSE | Edge RMSE | Halo | Hole | Seconds |
|---|---|---|---|---|---:|---:|---:|---:|---:|
| ZDefocus | 8 layers |  |  |  | 0.012373 | *0.03526* | 0.00031912 | 0.0035307 | 0.23316 |
| ZDefocus | 16 layers |  |  |  | 0.013297 | 0.041524 | 0.0006824 | 0.0031396 | 0.35938 |
| ZDefocus | 32 layers |  |  |  | 0.013574 | 0.043135 | 0.0007368 | 0.0030693 | 0.57329 |
| Niik-l | Unmix | on | Split | off | 0.010677 | 0.024011 | 0.0001167 | 0.003057 | 0.29032 |
| Niik-l | Unmix | on | Split | on | 0.010677 | 0.024011 | 0.0001167 | 0.003057 | 0.30006 |
| Niik-l | Unmix | off | Split | off | 0.01624 | 0.057543 | 5.9136e-05 | 0.005299 | 0.29942 |
| Niik-l | Unmix | off | Split | on | 0.01624 | 0.057543 | 5.9136e-05 | 0.005299 | 0.27809 |
| Niik-l | Snap | on | Split | off | 0.010633 | **0.02365** | 0.00011215 | 0.0033314 | 0.27339 |
| Niik-l | Snap | on | Split | on | 0.010633 | **0.02365** | 0.00011215 | 0.0033314 | 0.28002 |
| Niik-l | Snap | off | Split | off | 0.01633 | 0.058004 | 5.9627e-05 | 0.0054732 | 0.31376 |
| Niik-l | Snap | off | Split | on | 0.01633 | 0.058004 | 5.9627e-05 | 0.0054732 | 0.31555 |
| Niik-l | Off | on | Split | off | 0.01469 | 0.049354 | 9.5429e-05 | 0.0033837 | 0.31387 |
| Niik-l | Off | on | Split | on | 0.01469 | 0.049354 | 9.5429e-05 | 0.0033837 | 0.3352 |
| Niik-l | Off | off | Split | off | 0.017964 | 0.066243 | 5.8712e-05 | 0.0061596 | 0.29353 |
| Niik-l | Off | off | Split | on | 0.017964 | 0.066243 | 5.8712e-05 | 0.0061596 | 0.27942 |
| Niik-l | Unmix | on | Fast | off | 0.010677 | 0.024011 | 0.0001167 | 0.003057 | 0.31149 |

### grazingFloor

Edge band: 11628 pixels. Bold: lowest edge RMSE in the scene. Italic: lowest ZDefocus edge RMSE.

| Engine | Layers / edge mode | Fill | Occlusion edges | P/N | RMSE | Edge RMSE | Halo | Hole | Seconds |
|---|---|---|---|---|---:|---:|---:|---:|---:|
| ZDefocus | 8 layers |  |  |  | 0.011143 | **0.020349** | 0.00060765 | 0.0085876 | 0.51379 |
| ZDefocus | 16 layers |  |  |  | 0.015172 | 0.031808 | 0.0012822 | 0.0099621 | 0.89826 |
| ZDefocus | 32 layers |  |  |  | 0.016405 | 0.027623 | 0.0010375 | 0.01172 | 1.7206 |
| Niik-l | Unmix | on | Split | off | 0.023867 | 0.065936 | 0.0016364 | 0.0080033 | 0.94489 |
| Niik-l | Unmix | on | Split | on | 0.02398 | 0.066454 | 0.0016625 | 0.0080058 | 0.8832 |
| Niik-l | Unmix | off | Split | off | 0.10968 | 0.13405 | 1.0752e-05 | 0.16166 | 0.20024 |
| Niik-l | Unmix | off | Split | on | 0.10968 | 0.13404 | 1.0853e-05 | 0.16166 | 0.17056 |
| Niik-l | Snap | on | Split | off | 0.025239 | 0.072126 | 0.0019472 | 0.0079967 | 0.91678 |
| Niik-l | Snap | on | Split | on | 0.025239 | 0.072126 | 0.0019472 | 0.0079967 | 0.88725 |
| Niik-l | Snap | off | Split | off | 0.10975 | 0.13479 | 0.00010406 | 0.16177 | 0.15674 |
| Niik-l | Snap | off | Split | on | 0.10975 | 0.13479 | 0.00010406 | 0.16177 | 0.16302 |
| Niik-l | Off | on | Split | off | 0.025053 | 0.071299 | 0.0019161 | 0.0079765 | 0.93763 |
| Niik-l | Off | on | Split | on | 0.025053 | 0.071299 | 0.0019161 | 0.0079765 | 0.94752 |
| Niik-l | Off | off | Split | off | 0.10977 | 0.135 | 0.00010512 | 0.16181 | 0.16152 |
| Niik-l | Off | off | Split | on | 0.10977 | 0.135 | 0.00010512 | 0.16181 | 0.16758 |
| Niik-l | Unmix | on | Fast | off | 0.015123 | 0.024732 | 0.00013413 | 0.0062522 | 0.47521 |

### highlightsBehindEdge

Edge band: 15633 pixels. Bold: lowest edge RMSE in the scene. Italic: lowest ZDefocus edge RMSE.

| Engine | Layers / edge mode | Fill | Occlusion edges | P/N | RMSE | Edge RMSE | Halo | Hole | Seconds |
|---|---|---|---|---|---:|---:|---:|---:|---:|
| ZDefocus | 8 layers |  |  |  | 0.065136 | 0.11509 | 8.0332e-06 | 0.028725 | 0.17404 |
| ZDefocus | 16 layers |  |  |  | 0.06086 | 0.11379 | 9.4589e-06 | 0.026291 | 0.23151 |
| ZDefocus | 32 layers |  |  |  | 0.052663 | *0.11227* | 3.5087e-05 | 0.022233 | 0.21969 |
| Niik-l | Unmix | on | Split | off | 0.047671 | 0.10802 | 1.0037e-05 | 0.019563 | 0.23259 |
| Niik-l | Unmix | on | Split | on | 0.047671 | 0.10802 | 1.0037e-05 | 0.019563 | 0.2509 |
| Niik-l | Unmix | off | Split | off | 0.050823 | 0.12082 | 5.4896e-06 | 0.020131 | 0.22074 |
| Niik-l | Unmix | off | Split | on | 0.050823 | 0.12082 | 5.4896e-06 | 0.020131 | 0.23499 |
| Niik-l | Snap | on | Split | off | 0.047684 | 0.10808 | 1.5702e-05 | 0.019562 | 0.23255 |
| Niik-l | Snap | on | Split | on | 0.047684 | 0.10808 | 1.5702e-05 | 0.019562 | 0.2493 |
| Niik-l | Snap | off | Split | off | 0.050994 | 0.12149 | 5.31e-06 | 0.020116 | 0.21497 |
| Niik-l | Snap | off | Split | on | 0.050994 | 0.12149 | 5.31e-06 | 0.020116 | 0.24894 |
| Niik-l | Off | on | Split | off | 0.047616 | **0.10779** | 6.3188e-06 | 0.019564 | 0.26271 |
| Niik-l | Off | on | Split | on | 0.047616 | **0.10779** | 6.3188e-06 | 0.019564 | 0.23745 |
| Niik-l | Off | off | Split | off | 0.050993 | 0.12149 | 5.2821e-06 | 0.020116 | 0.20661 |
| Niik-l | Off | off | Split | on | 0.050993 | 0.12149 | 5.2821e-06 | 0.020116 | 0.25143 |
| Niik-l | Unmix | on | Fast | off | 0.047671 | 0.10802 | 1.0037e-05 | 0.019563 | 0.23364 |

### eclipsedBokeh

Edge band: 14338 pixels. Bold: lowest edge RMSE in the scene. Italic: lowest ZDefocus edge RMSE.

| Engine | Layers / edge mode | Fill | Occlusion edges | P/N | RMSE | Edge RMSE | Halo | Hole | Seconds |
|---|---|---|---|---|---:|---:|---:|---:|---:|
| ZDefocus | 8 layers |  |  |  | 0.076955 | 0.078657 | 0.00022705 | 0.025409 | 0.4385 |
| ZDefocus | 16 layers |  |  |  | 0.027489 | 0.064884 | 0.00027808 | 0.0090096 | 0.44055 |
| ZDefocus | 32 layers |  |  |  | 0.027362 | *0.064328* | 0.00025608 | 0.0090143 | 0.52582 |
| Niik-l | Unmix | on | Split | off | 0.025389 | **0.05536** | 3.178e-05 | 0.0090679 | 0.52998 |
| Niik-l | Unmix | on | Split | on | 0.025389 | **0.05536** | 3.178e-05 | 0.0090679 | 0.56905 |
| Niik-l | Unmix | off | Split | off | 0.030036 | 0.075587 | 1.7843e-05 | 0.0095493 | 0.54887 |
| Niik-l | Unmix | off | Split | on | 0.030036 | 0.075587 | 1.7843e-05 | 0.0095493 | 0.50454 |
| Niik-l | Snap | on | Split | off | 0.025599 | 0.056346 | 3.7462e-05 | 0.0090739 | 0.56733 |
| Niik-l | Snap | on | Split | on | 0.025599 | 0.056346 | 3.7462e-05 | 0.0090739 | 0.65371 |
| Niik-l | Snap | off | Split | off | 0.03001 | 0.075482 | 1.5292e-05 | 0.0095614 | 0.52947 |
| Niik-l | Snap | off | Split | on | 0.03001 | 0.075482 | 1.5292e-05 | 0.0095614 | 0.51843 |
| Niik-l | Off | on | Split | off | 0.025587 | 0.056287 | 3.7187e-05 | 0.009074 | 0.56946 |
| Niik-l | Off | on | Split | on | 0.025587 | 0.056287 | 3.7187e-05 | 0.009074 | 0.57568 |
| Niik-l | Off | off | Split | off | 0.030009 | 0.075477 | 1.5287e-05 | 0.0095613 | 0.50482 |
| Niik-l | Off | off | Split | on | 0.030009 | 0.075477 | 1.5287e-05 | 0.0095613 | 0.54543 |
| Niik-l | Unmix | on | Fast | off | 0.025389 | **0.05536** | 3.178e-05 | 0.0090679 | 0.57857 |
