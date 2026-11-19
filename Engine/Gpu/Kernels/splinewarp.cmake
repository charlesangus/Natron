slang_add_kernel(splinewarp Kernels/splinewarp.slang ENTRY splinewarp_main REGISTRY NatronGpu)
slang_add_kernel(splinewarp_chain Kernels/splinewarp.slang ENTRY splinewarp_main REGISTRY NatronGpu FLAGS -DWARP_CHAIN)
