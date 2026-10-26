# The noise core's test entry: noise4 at a buffer of split coordinates, for the CPU/GPU parity test.
# -fp-mode precise keeps slangc from contracting a * b + c, which the C++ core never fuses.
slang_add_kernel(noisecore_sample Kernels/noisecore_test.slang ENTRY noisecore_sample REGISTRY NatronGpu FLAGS -fp-mode precise)
