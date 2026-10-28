# The shape falloff library's test entry, for the CPU/GPU parity test.
# -fp-mode precise keeps slangc from contracting a * b + c, which the C++ never fuses.
slang_add_kernel(shapefalloff_test Kernels/shapefalloff_test.slang ENTRY shapefalloff_test_main REGISTRY NatronGpu FLAGS -fp-mode precise)
