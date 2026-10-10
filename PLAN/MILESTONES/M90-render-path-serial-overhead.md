# M90 - Render Path Serial Overhead

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Remove the single-threaded work around native node kernels that M84.P1.T1 measured.

- At UHD float on 16 threads, every Blur task spends about 100 ms in serial work that doesn't depend on σ or filter:
  - ~50 ms in a single-threaded `memmove` in `Image::pasteFromForDepth<float>`, called from `EffectInstance::renderHandler` after `render()` returns. It is probably the `tmpImage` → `renderMappedImage` paste around `Engine/EffectInstance.cpp:3163`, not yet pinned down. An `eu-stack` profile put 72% of busy samples there.
  - ~15 ms in `Blur::writeWindow`.
  - ~15 ms in other `renderRoI` work, such as image allocation.
- Because of this, IIR Blur is flat at ~19 ms/Mpx against ~3.4 ms/Mpx for the bare kernel, and FIR at σ3 is 12× its bare kernel. Grade carries about 5 ms/Mpx of the same overhead (9.4 ms/Mpx in-graph against 3.4 bare).
- The overhead caps every native node's in-graph parallelism at roughly 4–6 of 16 threads. It also skews the CPU side of M84 - GPU Placement And Residency's cost model.

The user decided on 2026-10-10 that a CPU-side fix bigger than a few lines in `Blur.cpp` gets its own milestone. M84 ships only the uninitialised-buffer change in `Blur.cpp` (σ25 25.1 → 21.0 ms/Mpx).

Blocked on: nothing; it can be elaborated whenever it's scheduled. It is independent of the GPU work. When it lands, M84's cost seed table (`Engine/GpuCostSeed.inc`) should be regenerated, if M84 has shipped by then.

Acceptance sketch:
- The paste after `render()` is either removed (render directly into the mapped image) or parallelised.
- In-graph UHD Blur and Grade at 16 threads come within ~1.5× of their bare-kernel ms/Mpx, measured with `graph_bench.py` `blurchain`/`chain` under `NATRON_RENDER_PROFILE`.
- No ctest regression in either scheduler mode.
