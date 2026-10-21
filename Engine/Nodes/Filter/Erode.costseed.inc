// GPU cost model seed rows for net.sf.cimg.CImgErode, written by tools/bench/render-profile-report.py
// --emit-cost-table from the inputs in Engine/GpuCostSeeds/inputs.txt. Engine/GpuCostModel.cpp describes the rows.
NATRON_GPU_COST_SEED_CPU_ROW("net.sf.cimg.CImgErode", 54.6268, 2.7995)
NATRON_GPU_COST_SEED_GPU_ROW("net.sf.cimg.CImgErode", "plain", 0.0, 0.8231, nullptr, 6)
NATRON_GPU_COST_SEED_GPU_ROW("net.sf.cimg.CImgErode", "plain", 0.0, 0.5027, nullptr, 1000000)
