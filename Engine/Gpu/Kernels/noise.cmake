# The Noise generator. -fp-mode precise keeps slangc from contracting a * b + c, which the C++ core
# never fuses.
slang_add_kernel(noise Kernels/noise.slang ENTRY noise_main REGISTRY NatronGpu FLAGS -fp-mode precise)
