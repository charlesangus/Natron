# M255.P2.T2 — MoonRay deep over Arras (verification of decision (d))

Probe: `moonray/run.sh`. It renders 256x256 at 64 spp through stock hd_moonray over a local Arras session in `natron-dev:6c97a956d295d586`.
- **Deep request:** `parameters:moonray:output_type=deep` on the `beauty` AOV, beside flat `color` and `alpha`.
- **Dump:** a shim loaded only into execComp dumps every output at frame complete, as batch would.
- **Comparison:** batch moonray renders the same scene. The log is `moonray/results/probe.log`.

## Answers
1. **The Arras progressive path fills DeepBuffer: yes.** It holds 31,304 samples, with the same per-pixel counts as batch (difference 0).
2. **McrtFbSender copes with a deep output beside flat ones: yes.** There is no crash, and every flat AOV arrives. Turning deep on changes `color` by RMSE 0.000085, probably because deep forces scalar mode.
3. **Weighting is correct, but MoonRay's deep holds the first surface only.** MoonRay records only the first surface each camera ray hits (`PathIntegrator.cc:1408-1426`), so anything seen through a partly transparent surface is missing from deep. Flattened deep vs beauty: RGBA RMSE 0.106. On the 64x64 run, 876 of 4096 pixels had less deep alpha than flat alpha. Progressive and batch match. This is a MoonRay limit, not an Arras one.
4. **Sizes:** 0.478 samples/px mean, max 3 (histogram 0:34472, 1:30831, 2:226, 3:7). At 1920x1080 that extrapolates to about 0.99 M samples, 30.6 MiB per frame (41.9 MiB with DCX masks), and about 1.8 MiB per 64-row band.

## DeepFrame sketch
- **Header:** frame/sync id, width/height, aperture and region, channel names (R G B A Z ZBack plus AOVs and ids), deep format, max samples, band index and count, row range.
- **Body:** per-pixel uint32 counts for the band, then packed float32 samples in Z order (plus a 64-bit subpixel mask and flags for DCX).
- **Normalising:** resolve the raw sums before sending, the same normalisation `DeepBuffer::write` applies, so the receiver copies samples straight into `HydraDeepBuffer`.

## Notes
- **Opacity:** `UsdPreviewSurface` opacity becomes refraction in MoonRay, not presence. Partial transparency needs `DwaBaseMaterial` presence, and the scene-index path.
- **Docker:** batch moonray in Docker needs `-auto_affinity off -mem_affinity off`.
- **Teardown:** tearing down the Hydra delegate after an Arras render crashes (the known teardown race).
- **Volumes:** volumes plus deep force scalar mode. Progressive volume deep samples may be replaced each pass rather than merged (read from source, untested).

## Verdict
Transport (d) works as designed, with small payloads. The open question is value: MoonRay deep gives fewer, less complete samples than Cycles deep (P2.T1).
