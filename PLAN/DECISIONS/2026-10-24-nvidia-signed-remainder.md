# NVIDIA: no signed remainder of a possibly negative value in kernels

On the NVIDIA 580 driver (RTX 3070 Ti), a SPIR-V `OpSRem` whose divisor is not a compile-time constant and whose dividend is negative returns the unsigned remainder of the dividend's bit pattern: -7 % 6 gives 3, where the SPIR-V spec, lavapipe and RADV give -1. A power-of-two divisor hides it. The behaviour was proven on hardware by swapping shader modules in M91 - NVIDIA GPU Validation, where it gave X-Trans RAW pixels left of or above the origin the wrong colour.

Decision: Slang kernels never apply `%` to a signed value that can be negative. Wrap with a remainder of non-negative operands, e.g. `v >= 0 ? uint(v) % p : (p - uint(-v) % p) % p`, which agrees with floor-modulo and with the CPU twins on every driver.
