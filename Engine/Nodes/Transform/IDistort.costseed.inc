// GPU cost model seed rows for IDistort, estimated from CornerPin's measured rows until a benchmark of
// IDistort replaces them: the same resampling per pixel, plus five map reads for the Jacobian.
// Engine/GpuCostModel.cpp describes the rows.
NATRON_GPU_COST_SEED_CPU_ROW("IDistort", 39.0, 4.2858)
NATRON_GPU_COST_SEED_GPU_ROW("IDistort", "plain", 0.0, 0.1500)
