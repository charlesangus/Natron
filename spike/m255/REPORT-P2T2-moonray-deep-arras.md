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

## Follow-up: first-surface limit confirmed and fixed (2026-11-16)
**The limit is deliberate in MoonRay, and only affects presence.** The evidence:
- `lib/rendering/pbr/handlers/ShadeBundleHandler.cc:211` says "first hit only for now", and line 566 drops deep data on presence continuation rays.
- In scalar mode, `PathIntegrator.cc` (~1408-1426, ~1486) adds one sample with the composite colour at the front depth.
- `Film.cc:441` hardcodes one deep layer, and no scene variable changes it.

Refraction and opaque surfaces were already correct. The earlier claim that deep forces scalar mode was wrong: only volumes combined with deep do (`RenderContext.cc:3465`).

**Patch:** `moonray/moonray-true-deep.patch`, +76/−12 lines in `PathIntegrator.cc/.h` and `ShadeBundleHandler.cc`; only `librendering_pbr.so` changes. Every surface in a presence chain gets its own deep record, with its own depth from the camera, colour and alpha. Built with `moonray/build.sh` (results in `moonray/results/true-deep/`).

Batch, 128x128 at 16 spp, flattened deep vs beauty RMSE (rgba):

| Variant | Before | After | Samples per covered px (after) |
|---|---|---|---|
| presence, vector | 0.106 | 0.00011 | 1.41 |
| presence, scalar | 0.137 | 0.00002 | 1.40 |
| presence, z tolerance 0.01 | 0.106 | 0.00011 | 4.02 |
| presence, DCX | 0.153 | 0.153 | (DCX writer doesn't premultiply; not fixed) |
| opaque / glass | ~0 | ~0 | 1.01 |

Hydra over Arras at 256x256 / 64 spp: flattened progressive deep vs Hydra colour RMSE is 0.00006, and no pixel has less deep alpha than flat alpha. There are 43,176 samples (0.66/px; histogram 1:19336, 2:11344, 3:384). Progressive and batch sample counts differ on 88 pixels, which is sampling noise.

**Remaining gaps:**
- the DCX writer;
- volumes behind presence;
- behind-surface scalar samples carry RGB only (no deep AOVs);
- MoonRay's default deep Z tolerance (2.0) merges layers less than 1 unit apart, so Natron should send a smaller value.
