// GPU cost model seed rows for net.sf.openfx.TransformPlugin, written by tools/bench/render-profile-report.py
// --emit-cost-table from the inputs in Engine/GpuCostSeeds/inputs.txt. Engine/GpuCostModel.cpp describes the rows.
NATRON_GPU_COST_SEED_CPU_ROW("net.sf.openfx.TransformPlugin", 26.1260, 4.2858)
NATRON_GPU_COST_SEED_CPU_MOTION_BLUR_ROW("net.sf.openfx.TransformPlugin", 996.1061, 2.8736)
NATRON_GPU_COST_SEED_GPU_ROW("net.sf.openfx.TransformPlugin", "plain", 0.0, 0.0808)
NATRON_GPU_COST_SEED_GPU_ROW("net.sf.openfx.TransformPlugin", "motion_blur", 0.0, 1.1803)
