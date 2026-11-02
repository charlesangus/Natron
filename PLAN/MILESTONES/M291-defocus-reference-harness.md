# M291 - Defocus Reference Harness

Measure before revisiting ZDefocus (user, 2026-11-01). Niik-l's fork (remote `niikl`, branch `niikl/RB-2.6`, `Engine/Dev/PostFX/DefocusCore.{h,cpp}`) replaced the gather with a scatter engine. That engine splits edge pixels into front and back samples ("Unmix"), forces slice boundaries at depth edges, fills behind the foreground with a distance transform, and claims 2–10x lower edge error than a gather. Its testbed isn't in their tree, so the claim can't be checked. This milestone builds our own ground truth and scores, on the same scenes, today's ZDefocus (M131 - Native Defocus Family, M133 - ZDefocus Lens Aberrations) against a test-only port of Niik-l's engine. The results decide what M294 - ZDefocus Engine Revisit adopts.

Deps: M131 - Native Defocus Family, M133 - ZDefocus Lens Aberrations (ZDefocus must be on main to be scored).

## Scope

**In:**
- An in-tree analytic thin-lens renderer (test code only). It renders each scene twice: pinhole (RGBA, depth, P, N) and Monte Carlo over a thin-lens aperture (the truth).
- A fixture scene set and an edge-error metric.
- Scores for ZDefocus and for a test-only port of Niik-l's DefocusCore, each with and without P/N and across its edge and fill modes.
- A results report shared with the user.

**Out:**
- Shipping any engine change: M294 - ZDefocus Engine Revisit.
- Debug outputs: M293 - ZDefocus Debug Outputs.
- Cycles or Hydra reference renders: no milestone (user picked analytic scenes only).
- Shipping Niik-l's engine as a node: no milestone (user, 2026-11-01: "their node is a bit rough around the edges").

**Edge cases:**
- **Determinism:** stratified sampling with a fixed seed, so a rerun gives bit-identical truth images.
- **Scene coverage:**
  - a sharp foreground over a blurred background;
  - a blurred foreground over a sharp background (the case where a gather shows holes);
  - an aliased depth edge (a pixel-coverage mix of two depths);
  - a grazing floor plane (where P/N should help);
  - emissive highlight points behind a foreground edge.
- **LFS:** scenes are generated at test time and nothing large is committed. Reference crops in the report are ≤256 px half-float.
- **P/N test (user):** the user suspects P/N rarely helps, so the report states the per-scene edge-error difference with and without P/N.

**Assumptions:**
- Unconfirmed: 512×288 per scene is enough to tell the engines apart, at 256 aperture samples per pixel for the truth.
- Unconfirmed: Niik-l's DefocusCore builds as plain C++17 with only light adaptation (its header says it has no Natron dependencies).

## Phase 291.1: Analytic reference renderer

- [ ] M291.P1.T1 — Write the analytic thin-lens renderer
  - files: `Tests/Native/DefocusReference/AnalyticScene.h`, `Tests/Native/DefocusReference/AnalyticScene.cpp`, `Tests/CMakeLists.txt` (add the sources to the native test target)
  - approach: Primitives are planes, discs, spheres and emissive points, each with a flat RGBA colour and an alpha for semi-transparent cards. Camera: focal length, f-stop, film back and focus distance, using the same CoC model as ZDefocus's Lens mode (`Engine/Nodes/Filter/DepthOfField.h`). `renderPinhole()` returns RGBA, planar depth, world P and N. `renderThinLens(samplesPerPixel, seed)` traces stratified aperture samples through the thin lens with nearest-hit compositing of alpha. Plain C++, no Natron types.
  - verify: `Tests/Native/DefocusReference_Test.cpp` (new): a plane at the focus distance renders the same pinhole and thin-lens (max abs diff < 1e-4); an emissive point off focus spreads to a disc whose radius matches `DepthOfField.h`'s CoC within half a pixel; the same seed gives identical output. `ctest -R DefocusReference` at the phase build.
  - size: M

- [ ] M291.P1.T2 — Define the fixture scenes
  - files: `Tests/Native/DefocusReference/Scenes.h`, `Tests/Native/DefocusReference/Scenes.cpp`
  - approach: Five named scenes per the Scope edge cases (sharpFgBlurBg, blurFgSharpBg, aliasedEdge, grazingFloor, highlightsBehindEdge). Each returns an `AnalyticScene` plus camera settings; aliasedEdge supersamples the pinhole depth 4x4 and averages it, as a renderer would.
  - verify: a test in `DefocusReference_Test.cpp` renders each scene's pinhole pass at 64×36 and checks that depth has the expected near/far range and that the alpha coverage is non-trivial.
  - size: M

## Phase 291.2: Metric and candidates

- [ ] M291.P2.T1 — Write the edge-error metric
  - files: `Tests/Native/DefocusReference/DefocusMetric.h`, `Tests/Native/DefocusReference/DefocusMetric.cpp`
  - approach: Compare a candidate RGBA with the truth. Report: global RMSE; RMSE inside an edge band (pixels within max CoC of a depth discontinuity in the pinhole depth); halo energy (positive error outside the truth's coverage); hole energy (negative error inside it). Return a struct, and write an optional diff crop as a half-float EXR through OIIO.
  - verify: tests in `DefocusReference_Test.cpp` show identical images score 0, and a one-pixel shift of an edge raises only the edge-band RMSE.
  - size: M

- [ ] M291.P2.T2 — Port Niik-l's DefocusCore as a test-only candidate
  - files: `Tests/Native/DefocusReference/niikl/DefocusCore.h`, `Tests/Native/DefocusReference/niikl/DefocusCore.cpp` (from `git show niikl/RB-2.6:Engine/Dev/PostFX/DefocusCore.{h,cpp}`), `Tests/CMakeLists.txt`
  - approach: Copy with the GPL header and attribution intact. Change only what is needed to compile in our test target (namespace, includes, any non-C++17 or Windows-only bits); leave the algorithm untouched. Do not port DefocusGpu or LensTrace. Expose an adapter `runNiiklDefocus(scene buffers, params)` taking RGBA, depth and optional P/N, and the edge mode (Off/Snap/Unmix), fill on/off and occlusionEdges (Split/Fast).
  - verify: the native test target builds; a test renders blurFgSharpBg at 64×36 through the adapter and gets a finite, non-black image whose alpha stays within [0, 1].
  - size: L

- [ ] M291.P2.T3 — Wire today's ZDefocus as a candidate
  - files: `Tests/Native/DefocusReference/ZDefocusCandidate.h`, `Tests/Native/DefocusReference/ZDefocusCandidate.cpp`
  - approach: Render ZDefocus through the native test harness (`Tests/Native/BaseTest.h`, `RenderBothWays.h`, as `NativeZDefocus_Test.cpp` does), feeding the pinhole RGBA and depth, with Lens mode set to the scene's camera so the CoC matches the truth. Cover layer counts 8, 16 and 32.
  - verify: a test renders sharpFgBlurBg at 64×36 and checks that the in-focus foreground matches the pinhole within 1e-3.
  - size: M

## Phase 291.3: Runs and report

- [ ] M291.P3.T1 — Write the harness runner
  - files: `Tests/Native/DefocusHarness_Test.cpp`
  - approach: A CI smoke case runs every scene × candidate configuration at 64×36 with 16 aperture samples and checks that all metrics are finite. A full mode, enabled by `NATRON_DEFOCUS_HARNESS_REPORT=<dir>`, runs at 512×288 with 256 samples and writes `results.csv`, a markdown table and diff crops of at most 256 px into `<dir>`. It records wall time per candidate.
  - verify: `ctest -R DefocusHarness` passes at the phase build; a local full run writes the CSV and crops.
  - size: M

- [ ] M291.P3.T2 — Run the full harness and write up the results
  - files: `docs/defocus/harness-results.md`, plus at most 10 crops under `docs/defocus/harness/` (half-float, ≤256 px)
  - approach: Run the full mode in natron-dev. Write a table per scene: ZDefocus at each layer count against Niik-l with Unmix, Snap and Off, fill on/off, and P/N on/off. Add a short reading: where each engine wins, whether P/N helps beyond the grazing floor, which ideas (edge split, forced slice boundaries, fill, scatter itself) account for the gap, and a note on GPU-residency feasibility for each idea (user requires GPU residency). End with a recommendation for M294 - ZDefocus Engine Revisit.
  - verify: the report exists, its numbers match `results.csv`, and the crops are within the LFS limits (`du -sh docs/defocus/harness`).
  - size: M

**Verification gate:** the full build and `ctest` pass, including `DefocusReference` and the `DefocusHarness` smoke case. The full harness report is shared with the user (SendUserFile), and the user chooses M294 - ZDefocus Engine Revisit's direction from it. Record that choice in this file's `## Decisions` and in M294's `Blocked on:` line.

## Decisions

- 2026-11-01 — **Measure first, then revisit (user, /cat-plan grill):** score ZDefocus and a Niik-l prototype against a ground truth before choosing an engine.
- 2026-11-01 — **Analytic in-tree renderer for the truth (user):** deterministic, fast and LFS-free, preferred over Cycles renders.
- 2026-11-01 — **Test P/N (user):** "Would be interested to see if the P/N thing actually helps much in many cases. I suspect not, but worth testing."
