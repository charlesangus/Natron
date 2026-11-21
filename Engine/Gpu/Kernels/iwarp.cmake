slang_add_kernel(iwarp Kernels/iwarp.slang ENTRY iwarp_main REGISTRY NatronGpu)
slang_add_kernel(iwarp_chain Kernels/iwarp.slang ENTRY iwarp_main REGISTRY NatronGpu FLAGS -DWARP_CHAIN)
