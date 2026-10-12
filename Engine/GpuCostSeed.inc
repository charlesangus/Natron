// Seed costs for the GPU placement cost model. Generated, do not edit.
//
// Serial (noRenderThreads=1) inputs, dated by file modification time:
//   build/bench/m82-profile/cg_multipass-serial.steady.jsonl (2026-10-10)
//   build/bench/m82-profile/defocus_retime-serial.steady.jsonl (2026-10-10)
//   build/bench/m82-profile/keying_grade-serial.steady.jsonl (2026-10-10)
// Threaded inputs:
//   build/bench/m82-profile/cg_multipass.steady.jsonl (2026-10-10)
//   build/bench/m82-profile/defocus_retime.steady.jsonl (2026-10-10)
//   build/bench/m82-profile/keying_grade.steady.jsonl (2026-10-10)
//
// Regenerate from the repository root with:
//   tools/bench/render-profile-report.py --serial-profile build/bench/m82-profile/cg_multipass.steady.jsonl=build/bench/m82-profile/cg_multipass-serial.steady.jsonl --serial-profile build/bench/m82-profile/defocus_retime.steady.jsonl=build/bench/m82-profile/defocus_retime-serial.steady.jsonl --serial-profile build/bench/m82-profile/keying_grade.steady.jsonl=build/bench/m82-profile/keying_grade-serial.steady.jsonl build/bench/m82-profile/cg_multipass.steady.jsonl build/bench/m82-profile/defocus_retime.steady.jsonl build/bench/m82-profile/keying_grade.steady.jsonl --emit-cost-table Engine/GpuCostSeed.inc
//
// Each macro expands to an initialiser list; the consuming code defines the row structs.
// CPU rows: { plugin id, serial ms per Mpx (median over tasks weighted by RoI pixels),
//   parallel speedup (serial over threaded ms per Mpx) }.
// CPU sigma rows: { plugin id, algorithm, largest sigma covered, serial ms per Mpx, parallel
//   speedup }. Use the first row of the algorithm whose sigma bound is at least the sigma.
//   Measured in situ on UHD float at 16 threads, so they are hard-coded here.
// GPU rows: { plugin id, largest sigma covered (0 when the kernel has no sigma), kernel-only
//   ms per Mpx on an RX 7900 XTX, from the UHD kernel times of the GPU compute backend
//   design notes, section 2.3 }.

#define NATRON_GPU_COST_SEED_CPU_ROWS \
    { "fr.inria.built-in.Write", 16.0285, 1.1214 }, \
    { "net.sf.cimg.CImgBlur", 46.9759, 2.3127 }, \
    { "net.sf.cimg.CImgDilate", 50.1112, 2.3281 }, \
    { "net.sf.cimg.CImgErode", 54.6268, 2.7995 }, \
    { "net.sf.openfx.CheckerBoardPlugin", 3.8872, 0.8641 }, \
    { "net.sf.openfx.ChromaKeyerPlugin", 63.6806, 1.6470 }, \
    { "net.sf.openfx.ColorCorrectPlugin", 39.6044, 3.2192 }, \
    { "net.sf.openfx.Despill", 9.0678, 0.8832 }, \
    { "net.sf.openfx.FrameBlend", 20.6113, 0.9152 }, \
    { "net.sf.openfx.GradePlugin", 13.5443, 1.1202 }, \
    { "net.sf.openfx.KeyerPlugin", 17.9119, 1.3066 }, \
    { "net.sf.openfx.MergePlugin", 5.8797, 1.0836 }, \
    { "net.sf.openfx.Radial", 8.7415, 0.5146 }, \
    { "net.sf.openfx.Ramp", 5.1916, 0.6209 }, \
    { "net.sf.openfx.Rectangle", 4.5997, 0.5637 }, \
    { "net.sf.openfx.Retime", 0.0008, 0.3011 }, \
    { "net.sf.openfx.TimeBlur", 0.1879, 0.9461 }, \
    { "net.sf.openfx.TransformPlugin", 996.1061, 2.8736 }

#define NATRON_GPU_COST_SEED_CPU_SIGMA_ROWS \
    { "net.sf.cimg.CImgBlur", "fir", 3.0, 46.0000, 3.4848 }, \
    { "net.sf.cimg.CImgBlur", "fir", 25.0, 100.0000, 4.7619 }, \
    { "net.sf.cimg.CImgBlur", "fir", 1e9, 267.0000, 6.1521 }, \
    { "net.sf.cimg.CImgBlur", "iir", 1e9, 71.0000, 3.7566 }

#define NATRON_GPU_COST_SEED_GPU_ROWS \
    { "net.sf.openfx.GradePlugin", 0.0, 0.0482 }, \
    { "net.sf.cimg.CImgBlur", 3.0, 0.3979 }, \
    { "net.sf.cimg.CImgBlur", 25.0, 2.5921 }, \
    { "net.sf.cimg.CImgBlur", 1e9, 6.3296 }
