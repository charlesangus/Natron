# The shape generators: Ramp, Radial, Rectangle and ColorWheel. -fp-mode precise keeps slangc from
# contracting a * b + c, which the C++ never fuses.
slang_add_kernel(ramp Kernels/shapes.slang ENTRY ramp_main REGISTRY NatronGpu FLAGS -fp-mode precise)
slang_add_kernel(radial Kernels/shapes.slang ENTRY radial_main REGISTRY NatronGpu FLAGS -fp-mode precise)
slang_add_kernel(rectangle Kernels/shapes.slang ENTRY rectangle_main REGISTRY NatronGpu FLAGS -fp-mode precise)
slang_add_kernel(colorwheel Kernels/shapes.slang ENTRY colorwheel_main REGISTRY NatronGpu FLAGS -fp-mode precise)
