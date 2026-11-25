slang_add_kernel(stmap Kernels/stmap.slang ENTRY stmap_main REGISTRY NatronGpu)
slang_add_kernel(idistort Kernels/stmap.slang ENTRY idistort_main REGISTRY NatronGpu)
slang_add_kernel(stmap_chain Kernels/stmap.slang ENTRY stmap_main REGISTRY NatronGpu FLAGS -DWARP_CHAIN)
slang_add_kernel(idistort_chain Kernels/stmap.slang ENTRY idistort_main REGISTRY NatronGpu FLAGS -DWARP_CHAIN)
