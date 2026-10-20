// Seed costs for the GPU placement cost model. Generated, do not edit.
//
// Serial (noRenderThreads=1) inputs, dated by file modification time:
//   build/bench/m82-profile/cg_multipass-serial.steady.jsonl (2026-10-10)
//   build/bench/m82-profile/defocus_retime-serial.steady.jsonl (2026-10-10)
//   build/bench/m82-profile/keying_grade-serial.steady.jsonl (2026-10-10)
//   build/bench/m85-nodes/invert-serial.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/clamp-serial.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/add-serial.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/multiply-serial.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/gamma-serial.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/saturation-serial.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/colorlookup-serial.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/constant-serial.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/solid-serial.steady.jsonl (2026-10-14)
// Threaded inputs:
//   build/bench/m82-profile/cg_multipass.steady.jsonl (2026-10-10)
//   build/bench/m82-profile/defocus_retime.steady.jsonl (2026-10-10)
//   build/bench/m82-profile/keying_grade.steady.jsonl (2026-10-10)
//   build/bench/m85-nodes/invert.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/clamp.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/add.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/multiply.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/gamma.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/saturation.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/colorlookup.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/constant.steady.jsonl (2026-10-14)
//   build/bench/m85-nodes/solid.steady.jsonl (2026-10-14)
//
// Regenerate from the repository root with:
//   tools/bench/render-profile-report.py --serial-profile build/bench/m82-profile/cg_multipass.steady.jsonl=build/bench/m82-profile/cg_multipass-serial.steady.jsonl --serial-profile build/bench/m82-profile/defocus_retime.steady.jsonl=build/bench/m82-profile/defocus_retime-serial.steady.jsonl --serial-profile build/bench/m82-profile/keying_grade.steady.jsonl=build/bench/m82-profile/keying_grade-serial.steady.jsonl --serial-profile build/bench/m85-nodes/invert.steady.jsonl=build/bench/m85-nodes/invert-serial.steady.jsonl --serial-profile build/bench/m85-nodes/clamp.steady.jsonl=build/bench/m85-nodes/clamp-serial.steady.jsonl --serial-profile build/bench/m85-nodes/add.steady.jsonl=build/bench/m85-nodes/add-serial.steady.jsonl --serial-profile build/bench/m85-nodes/multiply.steady.jsonl=build/bench/m85-nodes/multiply-serial.steady.jsonl --serial-profile build/bench/m85-nodes/gamma.steady.jsonl=build/bench/m85-nodes/gamma-serial.steady.jsonl --serial-profile build/bench/m85-nodes/saturation.steady.jsonl=build/bench/m85-nodes/saturation-serial.steady.jsonl --serial-profile build/bench/m85-nodes/colorlookup.steady.jsonl=build/bench/m85-nodes/colorlookup-serial.steady.jsonl --serial-profile build/bench/m85-nodes/constant.steady.jsonl=build/bench/m85-nodes/constant-serial.steady.jsonl --serial-profile build/bench/m85-nodes/solid.steady.jsonl=build/bench/m85-nodes/solid-serial.steady.jsonl build/bench/m82-profile/cg_multipass.steady.jsonl build/bench/m82-profile/defocus_retime.steady.jsonl build/bench/m82-profile/keying_grade.steady.jsonl build/bench/m85-nodes/invert.steady.jsonl build/bench/m85-nodes/clamp.steady.jsonl build/bench/m85-nodes/add.steady.jsonl build/bench/m85-nodes/multiply.steady.jsonl build/bench/m85-nodes/gamma.steady.jsonl build/bench/m85-nodes/saturation.steady.jsonl build/bench/m85-nodes/colorlookup.steady.jsonl build/bench/m85-nodes/constant.steady.jsonl build/bench/m85-nodes/solid.steady.jsonl --emit-cost-table Engine/GpuCostSeed.inc
//
// Each macro expands to an initialiser list; the consuming code defines the row structs.
// CPU rows: { plugin id, serial ms per Mpx (median over tasks weighted by RoI pixels),
//   parallel speedup (serial over threaded ms per Mpx) }.
// CPU motion-blur rows: the same, for the plugins the bench comps render with motion blur;
//   they price only a task that renders with motion blur.
// CPU sigma rows: { plugin id, algorithm, largest sigma covered, serial ms per Mpx, parallel
//   speedup }. Use the first row of the algorithm whose sigma bound is at least the sigma.
//   Measured in situ on UHD float at 16 threads, so they are hard-coded here.
// GPU rows: { plugin id, kind (plain, iir or motion_blur), largest sigma covered (0 when the
//   kernel has no sigma), kernel-only ms per Mpx on an RX 7900 XTX, from the kernelMs of lone-node
//   regions at UHD, then optionally the Blur filter (nullptr for any) and the largest Erode or
//   Dilate size covered (0 for any) }. A row that names a filter or a size beats one that does not.

#define NATRON_GPU_COST_SEED_CPU_ROWS \
    { "fr.inria.built-in.Write", 16.0285, 1.1214 }, \
    { "net.sf.cimg.CImgBlur", 46.9759, 2.3127 }, \
    { "net.sf.cimg.CImgDilate", 50.1112, 2.3281 }, \
    { "net.sf.cimg.CImgErode", 54.6268, 2.7995 }, \
    { "net.sf.openfx.AddPlugin", 9.3236, 4.0218 }, \
    { "net.sf.openfx.CheckerBoardPlugin", 3.8872, 0.8641 }, \
    { "net.sf.openfx.ChromaKeyerPlugin", 63.6806, 1.6470 }, \
    { "net.sf.openfx.Clamp", 12.3939, 4.8673 }, \
    { "net.sf.openfx.ColorCorrectPlugin", 39.6044, 3.2192 }, \
    { "net.sf.openfx.ColorLookupPlugin", 20.3472, 5.4572 }, \
    { "net.sf.openfx.ConstantPlugin", 4.7901, 1.3197 }, \
    { "net.sf.openfx.Despill", 9.0678, 0.8832 }, \
    { "net.sf.openfx.FrameBlend", 20.6113, 0.9152 }, \
    { "net.sf.openfx.GammaPlugin", 19.7045, 4.8891 }, \
    { "net.sf.openfx.GradePlugin", 11.4800, 7.8630 }, \
    { "net.sf.openfx.Invert", 9.5062, 4.0413 }, \
    { "net.sf.openfx.KeyerPlugin", 17.9119, 1.3066 }, \
    { "net.sf.openfx.MergePlugin", 5.8797, 1.0836 }, \
    { "net.sf.openfx.MultiplyPlugin", 9.3583, 3.9996 }, \
    { "net.sf.openfx.Radial", 8.7415, 0.5146 }, \
    { "net.sf.openfx.Ramp", 5.1916, 0.6209 }, \
    { "net.sf.openfx.Rectangle", 4.5997, 0.5637 }, \
    { "net.sf.openfx.Retime", 0.0008, 0.3011 }, \
    { "net.sf.openfx.SaturationPlugin", 11.3752, 4.5915 }, \
    { "net.sf.openfx.Solid", 4.8460, 1.3508 }, \
    { "net.sf.openfx.TimeBlur", 0.1879, 0.9461 }, \
    { "net.sf.openfx.TransformPlugin", 26.1260, 4.2858 }

#define NATRON_GPU_COST_SEED_CPU_MOTION_BLUR_ROWS \
    { "net.sf.openfx.TransformPlugin", 996.1061, 2.8736 }

#define NATRON_GPU_COST_SEED_CPU_SIGMA_ROWS \
    { "net.sf.cimg.CImgBlur", "fir", 3.0, 30.1300, 6.4106 }, \
    { "net.sf.cimg.CImgBlur", "fir", 25.0, 84.6000, 6.9458 }, \
    { "net.sf.cimg.CImgBlur", "fir", 1e9, 252.8000, 7.2937 }, \
    { "net.sf.cimg.CImgBlur", "iir", 1e9, 52.8400, 7.7365 }

#define NATRON_GPU_COST_SEED_GPU_ROWS \
    { "net.sf.openfx.GradePlugin", "plain", 0.0, 0.0599 }, \
    { "net.sf.openfx.ColorCorrectPlugin", "plain", 0.0, 0.0620 }, \
    { "net.sf.openfx.Invert", "plain", 0.0, 0.0575 }, \
    { "net.sf.openfx.Clamp", "plain", 0.0, 0.0596 }, \
    { "net.sf.openfx.AddPlugin", "plain", 0.0, 0.0579 }, \
    { "net.sf.openfx.MultiplyPlugin", "plain", 0.0, 0.0581 }, \
    { "net.sf.openfx.GammaPlugin", "plain", 0.0, 0.0587 }, \
    { "net.sf.openfx.SaturationPlugin", "plain", 0.0, 0.0581 }, \
    { "net.sf.openfx.ColorLookupPlugin", "plain", 0.0, 0.0608 }, \
    { "net.sf.openfx.KeyerPlugin", "plain", 0.0, 0.0532 }, \
    { "net.sf.openfx.ChromaKeyerPlugin", "plain", 0.0, 0.0620 }, \
    { "net.sf.cimg.CImgErode", "plain", 0.0, 0.8231, nullptr, 6 }, \
    { "net.sf.cimg.CImgErode", "plain", 0.0, 0.5027, nullptr, 1000000 }, \
    { "net.sf.cimg.CImgDilate", "plain", 0.0, 0.8337 }, \
    { "net.sf.openfx.ConstantPlugin", "plain", 0.0, 0.0627 }, \
    { "net.sf.openfx.Solid", "plain", 0.0, 0.0615 }, \
    { "net.sf.openfx.CheckerBoardPlugin", "plain", 0.0, 0.0663 }, \
    { "net.sf.openfx.TransformPlugin", "plain", 0.0, 0.0808 }, \
    { "net.sf.openfx.TransformPlugin", "motion_blur", 0.0, 1.1803 }, \
    { "net.sf.cimg.CImgBlur", "plain", 3.0, 1.1470 }, \
    { "net.sf.cimg.CImgBlur", "plain", 25.0, 0.8527 }, \
    { "net.sf.cimg.CImgBlur", "plain", 1e9, 2.2600 }, \
    { "net.sf.cimg.CImgBlur", "iir", 1e9, 2.4130 }, \
    { "net.sf.cimg.CImgBlur", "plain", 1e9, 2.6886, "box", 0 }, \
    { "net.sf.cimg.CImgBlur", "plain", 1e9, 3.8870, "triangle", 0 }, \
    { "net.sf.cimg.CImgBlur", "plain", 1e9, 4.7803, "quadratic", 0 }, \
    { "net.sf.cimg.CImgBlur", "iir", 1e9, 3.4855, "gaussian", 0 }, \
    { "net.sf.cimg.CImgBlur", "iir", 1e9, 4.2040, "quasi_gaussian", 0 }
