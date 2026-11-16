# M255.P2.T1 — Cycles in-process deep

**Feasible, and cheap.** The pinned Cycles fork (charlesangus/cycles 90d7cb7, natron/deep-5.2) already carries the whole deep kernel. It is Niik-l's port (6ed56ae, cherry-picked from Niik-l 787be06, +2047/−13; then 7fa747e, cd828a6, c8d449e). Niik-l's commit says it ports the Cycles side of Blender PR #154410, which itself could not be fetched (GitHub 404, projects.blender.org unreachable).

The new patch, `deep/cycles-deep.patch`, is **+194/−6 lines over 6 files**:
- Cycles core, `path_trace.cpp` +24/−1: Film `use_deep_output` creates and sizes the deep driver, and a reset clears it.
- hdCycles, `output_driver.cpp`, `session.cpp/.h` and `render_delegate.cpp`, +130/−5: when a render completes, a `HydraDeepBuffer` is built and handed out through `HdRenderBuffer::GetResource()`.
- `HydraDeepBuffer.h` +40: a stand-in until M286 - Utility Renderer Deep Output defines the real header.

Deep is requested by binding the AOV `cycles:deep` (descriptor Float32Vec4). `deep_max_samples`, `deep_merge_threshold` and `deep_peel` stay `cycles:film:*` settings.

## Run (CPU, `ghcr.io/charlesangus/natron-dev:6c97a956d295d586`)
- Build: `DEEP_JOBS=2 ./build.sh`, 17 min, OK (`deep/results/build-tail.log`).
- Probe: `./run.sh` renders 256x256 at 16 samples: three tinted transparent cards and a sphere (`deep/results/probe.log`).

| | Flat | Deep |
|---|---|---|
| Time to converged (round 2) | 1.82 s | 2.32 s (1.27x) |
| Peak memory | 4.6 MB | 139 MB (kernel buffer 128 MB) |

- `GetResource()` returns `shared_ptr<HydraDeepBuffer const>`, with channels R G B A Z ZBack.
- Samples per pixel: min 0, mean 1.31, max 16 (mean 2.81 over covered pixels), 86,026 in total, sorted front to back, none at the 64-slot cap.
- Bytes per pixel: 43.5 handed out; **2,052 reserved by the kernel** (64 slots × 32 B). That is about 4.2 GB at 1920x1080.
- Turning deep on does not change the colour AOV (diff 0).
- Flattening the deep samples vs the colour AOV: mean |diff| R 0.016, G 0.005, B 0.008, A 0.0001; max R 0.23.
- Images: `deep/results/deep_flat.png` (flattened deep), `color.png`, `deep_count.png`.

## Findings to carry into M257 - Cycles Deep Output
1. **Kernel memory is the main problem.** The fixed 64-slot slab per pixel (4.2 GB at 1080p) is reserved whatever the scene needs. M257 needs a two-pass count-then-allocate, smaller slots, or tiles.
2. **Over-split curved surfaces.** The sphere yields 4–6 samples at nearly the same Z (8.0435–8.0489) in one pixel, because the 0.001 merge threshold is absolute and sub-pixel depth variation exceeds it. A relative or pixel-footprint-based threshold would cut the sample count.
3. **ZBack equals Z for surfaces.** That is expected; volumes would set it, but the scene has none.
4. **Flatten mismatch** (max 0.23 in red): per-sample colour is resolved against the beauty alpha. Investigate before shipping; a denoised beauty pass would make it worse.
5. **Flattened pixels missing.** The patched AOV's own flattened pixels came back empty (size 0), so clients that don't read deep would get nothing. Fix in M257.
6. **No object or prim ids** in deep. Add them in M257 for DeepHoldout and mattes.
7. **Spurious warning.** The `elementId` warning appears on every render (see the P1.T1 report).

Feasibility verdict: **yes, in-process.** The production work is memory, the merge threshold, ids and the flatten correctness, not the transport.
