# Noise library spike: FastNoise2 port (A) against linked FastNoise2 (B)

Run 2026-10-25 on the GPU host (RX 7900 XTX, RADV; AVX-512 CPU). Spike code: `tools/spikes/noise/` on the noise branch (`fn2port/` is A, `fn2lib/` is B, `fnl/` is the FastNoiseLite floor).

## Results (ms per megapixel, seeds 0 and 100000 agree)

| Noise | GPU | A v3, 1 thread | A v3, all cores | B strict, all cores | B relaxed, all cores | FastNoiseLite, 1 thread |
|---|---|---|---|---|---|---|
| Perlin fBm | 0.26 | 19.5 | 2.4 | 2.4-2.6 | 2.0-2.4 | 54-56 |
| SuperSimplex fBm | 0.41-0.46 | 43 | 5.4 | 4.9 | 3.4 | 75-77 |
| Cellular F1 fBm | 0.73-0.96 | 162 (fma) | 21 | 25 | 17 | 214 |

- A against the GPU: within 4.8e-7 (A without FMA bit-equal to the GPU kernel without FMA on most types).
- B strict against A: within 4.2e-7. B relaxed against A: up to 8e-5.
- A built for baseline x86-64 with explicit `fma` is pathological (cellular fBm 5749 ms/Mpx single-thread): no hardware FMA, so `fma` is a libm call.

## Comparison

- **CPU speed:** A is 2.5-3 times faster than FastNoiseLite and level with B strict; B relaxed is up to about 30% faster than A.
- **CPU/GPU parity:** A matches the GPU by construction; B strict also lands within 5e-7, B relaxed does not.
- **Maintenance:** A is one implementation mirrored line for line in C++ and Slang; B is FastNoise2 plus FastSIMD on the CPU and our Slang port on the GPU.
- **Build cost:** B vendors FastSIMD and its runtime SIMD dispatch; A needs only our own header.

## Decision

Option A (user, 2026-10-25). The CPU core must not run baseline x86-64 code with `fma` calls: build the inner loops for x86-64-v3 with a runtime dispatch to a baseline build compiled with `-ffp-contract=off` and no explicit `fma`, or keep FMA out of the core entirely; P2.T1 picks one and measures it.
