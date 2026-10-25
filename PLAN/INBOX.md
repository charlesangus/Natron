## Pending

### 2026-10-25T12:00:00Z — info
- refs: Engine/Knob.cpp setExpressionInternal; found during M92
`setExpressionInternal` rethrows a caught `const std::exception&` with `throw e;`, slicing a `std::runtime_error` to a plain `std::exception` and dropping its type. Use `throw;`.

### 2026-10-25T06:30:00Z — info
- refs: M167
A Switch's per-file key/value metadata (custom header attributes) always comes from its input 0, not the input it routes to at that frame, so a Write over a keyed Switch writes the first source's custom attributes into every file. Found while writing M167 P5.T4 (NativeWriteMetadata EachFileOfASequenceTakesItsOwnFramesFormat checks only size and PAR). Fix belongs in Switch / NativeEffectBase::getOutputMetadata routing; outside M167's files.

### 2026-10-25T12:00:00Z — change-request
- refs: none
- From M168 - NVIDIA Viewer Hand-off Validation (bosley-faux). New milestone suggested ("Startup Pane Paint On NVIDIA"): on NVIDIA GL under KWin (RTX 3070 Ti, driver 580.178.04, :0), a newly started Natron window shows its panes unpainted (#202020) on screen for ~16–18 s after map; only the menu bar and left toolbar draw. Happens with NATRON_GPU=off too, never under Xvfb. Screenshot `.local/m168/diag/run1-live.png`. User-visible startup paint issue, not the GPU hand-off.

### 2026-10-25T12:00:01Z — change-request
- refs: none
- From M168 - NVIDIA Viewer Hand-off Validation. Small test fixes for M107 - GPU Viewer Hand-off's UAT: (1) "the viewer shows the comp" in `Tests/gui/m107_viewer_handoff.py` checks only non-black, so an unpainted #202020 viewer passes; use `looks_blank`. (2) The UAT docstring and `Tests/gui/run-gui-test.sh`'s header should say host-display runs (GUI_TEST_HOST_DISPLAY) need a CPU reference taken on the same display. (3) Cosmetic: `tools/gpu-spike/desktop-handoff-check.sh` warns "GL renderer is not radeonsi" on NVIDIA.

### 2026-10-25T15:30:00Z — change-request
- refs: M129, M129.P1.T1, M129.P1.T2, M129.P1.T3, M129.P2.T1
- **M129 - Clean-Sheet Noise Node: FastNoise2 replaces the spike's candidates (user, /cat-discuss 2026-10-25).** FastNoise2 (github.com/Auburn/FastNoise2, MIT, same author as FastNoiseLite) has full 4D on every generator: Perlin, Value, Simplex, SuperSimplex, cellular value/distance (F1–F4 combinations, Euclidean/Manhattan/hybrid metrics), white, FBm/Ridged/PingPong fractals, and gradient/simplex domain warps, plus progressive/independent fractal warp. The milestone's note "FastNoiseLite is ruled out: 2D/3D only" is true of Lite only. MIT licensing removes the licence question (no GPLv3 consequence). Cycles and OpenSimplex2 are dropped as candidates. The spike now chooses between two ways of integrating FastNoise2, not between libraries. Apply to the milestone file:
  1. **Candidate sources:** add FastNoise2 as the chosen source with the facts above. Its kernels (`include/FastNoise/Generators/*.inl`) are scalar-looking code over FastSIMD vector types (`float32v`/`int32v`/masks), and gradients come from hash bit tricks with no lookup tables. FastSIMD is fetched at configure time through CPM, so linking it means vendoring FastSIMD. Relaxed mode uses `rsqrt`/`rcp` approximations. Mark Cycles and OpenSimplex2 as considered and dropped.
  2. **Replace P1.T1 (keep the ID):** "Port FastNoise2's 4D kernels to plain C++ and Slang (option A)". files: `tools/spikes/noise/fn2port/` (new: `fn2port4d.h`, `fn2port4d.slang`, `LICENSE`), `tools/spikes/noise/bench.cpp` (new). approach:
     - Port 4D Perlin, SuperSimplex and cellular distance F1 (Euclidean) plus FBm, line for line, into one C-like header and its Slang mirror. Map `float32v`→`float`, `int32v`→`int`, masks→`bool`, `FS::Select`→`select`, `Masked*`→ternaries, and `FMulAdd`→explicit `fma` on both sides.
     - Use exact `InvSqrt`/`Reciprocal`. Check `FS::Convert`'s float→int rounding and match it.
     - Write the CPU row loop so it autovectorises (SoA, no branches in the inner loop, `-O2 -march` baseline matching the build, no fast-math, `-ffp-contract=off` or explicit `fma`), and check the vectoriser report.
     - Harness: 2K tiles on CPU and GPU through the existing Vulkan test fixture.
     verify: the harness reports CPU ms/Mpx (single-thread and all cores), GPU ms/Mpx, max abs CPU/GPU difference, and the vectoriser report, and writes EXR samples to `build/spikes/noise/`. size: L.
  3. **Replace P1.T2 (keep the ID):** "Link FastNoise2 for the CPU, and benchmark A against B and FastNoiseLite (option B)". files: `tools/spikes/noise/fn2lib/` (new: build glue for FastNoise2 plus vendored FastSIMD at pinned revisions), `tools/spikes/noise/bench.cpp`. approach:
     - Build FastNoise2 with FastSIMD vendored at pinned revisions (no CPM network fetch), both in strict and in relaxed mode.
     - Render the same types and parameters through `GenPositionArray4D`, one call per row.
     - Benchmark three CPU paths on identical inputs: the P1.T1 port (A), FastNoise2's tuned SIMD (B), and FastNoiseLite's scalar 3D equivalents as the floor.
     - Measure A's and B's difference from each other and B's difference from P1.T1's GPU output.
     verify: the harness reports ms/Mpx (single-thread and all cores) for A, B (strict and relaxed) and FastNoiseLite, plus max abs differences A↔B and B↔GPU. size: L.
  4. **P1.T3 approach:** compare A against B, not libraries. Criteria:
     - **CPU speed (user):** A doesn't need every ounce of B's performance, but it must be faster than FastNoiseLite. The user expects a carefully written loop to come close to B.
     - **CPU/GPU parity:** A has parity by construction. For B, measure the gap.
     - **Maintenance:** one implementation (A) or two (B).
     - **Build cost:** vendoring FastSIMD and its runtime SIMD dispatch.
     Recommend A or B and checkpoint with the user. Licence is settled (MIT). Record the decision and fill in P2.T1's file list.
  5. **P2.T1 approach:** the core is FastNoise2-derived (option A: our ported header; option B: vendored FastNoise2+FastSIMD on the CPU with the Slang port for the GPU), per P1.T3. Add the types FastNoise2 lacks: hybrid multifractal, true cellular distance to edge (FastNoise2's F2−F1 only approximates it) and smooth F1. Write them ourselves; copy no Apache-2.0 code. Billow/turbulence is FBm over `Abs`.
  6. **Scope, In:** add a loop-period knob for seamless time loops. A 2D pattern loops through 4D (x, y, r·cos t, r·sin t), with the note that a looping 3D pattern would need 5D and is out. **Scope, Out:** a NoiseTool-encoded FastNoise2 node-tree knob, since the GPU would need a tree interpreter (no milestone; compose with several Noise nodes and a Merge instead). **Edge cases:** 4D cellular checks 81 cells per sample per octave, so the spike times it on the GPU.
  7. **Decisions:** append "2026-10-25 — FastNoise2 is the source (user): MIT and full 4D. The spike decides between porting its kernels to C++/Slang (A) and linking its SIMD library on the CPU with a Slang port (B). A must beat FastNoiseLite on the CPU but need not match FastNoise2's tuned SIMD." Amend the 2026-10-13 FastNoiseLite bullet to note that FastNoise2 is 4D. The 2026-10-13 spike decision is superseded: the Cycles and OpenSimplex2 candidates and their licence choice are dropped.

### 2026-10-25T15:00:00Z — change-request
- refs: none
- From M169 - Pipelined Strip Member Timing (bosley-faux). Two follow-ups: (1) Flake: `GpuResidencyTest.RegionMembersAreTimedOnTheDevice` failed `EXPECT_GT(blurNs, gradeNs)` in whole mode once in 10 NVIDIA runs (Grade 0.260 ms vs Blur 0.209 ms, first region of the process, no transfer overlap). (2) Spike-robust GPU calibration: 2–16× slow member intervals with no transfer overlap appear in every mode on a shared desktop GPU (22–50% utilisation from other programs) and swing `GpuCostModel`'s EMA; consider outlier rejection or a robust estimator. Data: `.local/m169/p1/` on bosley-faux.
