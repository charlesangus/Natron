# Geometry Kernel Execution Spike: Interpreter Or LLVM JIT

M360 - Geometry Kernel Language, P1.T1. This spike compares a batched bytecode interpreter with an LLVM JIT. Both run behind one small front end over M54 - Native Geometry Data Model's page layout: 1024-element pages, AoS float3 tuples, and fresh pages on write.

Prototype: `tools/spikes/geo-kernel/` on `milestone/m360-geometry-kernel-language`. It is uncommitted, standalone and not built by default. Run it with `tools/spikes/geo-kernel/run.sh` inside the natron-dev image. That one command builds and runs both backends, then the x86-64-v3 interpreter build, then the LLVM size measurements.

## How the numbers were taken

- **Host:** the dev host, an Intel N100 (Gracemont, AVX2, 4 cores, 15 GB).
- **Contention:** a `ninja -j3` Natron build was running in natron-dev the whole time. Load average was about 5, so treat differences under about 20 % as noise.
- **Workload:** 10M points on input 0 and 1M points on input 1, 4 threads with dynamic page claiming, 3 repetitions.
- **Correctness:** every backend matches a hand-written C++ reference bit for bit on every snippet. Contraction is off everywhere.
- **Not yet run on the GPU host.** The task asks for that run, and the same command produces it there.
- **Interpreter:** compiled at -O2 for the x86-64 baseline Natron ships, and again for x86-64-v3.
- **JIT:** LLVM 21.1.8 at O2, targeting the host CPU, linked against the distribution's release `libLLVM`. The image's own LLVM development package is an assertions build, and its compile times are 1.3-2x higher (23-157 ms).

## Runtime per snippet

Warm figures are ns per point at the minimum of 3 runs, with output pages preallocated. End-to-end (e2e) figures include allocating fresh output pages; they track warm within noise, apart from the page allocation for `add`.

| Snippet | Bytecode ops | Native C++ | Interp baseline (64 / 256 / 1024 lanes) | Interp v3, best | JIT | e2e min ms, interp best / JIT |
|---|---|---|---|---|---|---|
| add: `@P.y += 1` | 1 | 4.2 | 4.6 / 4.3 / 4.6 | 3.9 | 3.7 | 41 / 33 |
| maths: length, sin, `@ptnum` | 18 | 27.0 | 24.6 / 22.9 / 26.1 | 21.8 | 22.2 | 222 / 240 |
| noise displace | 15 | 148 | 155 / 166 / 161 | 153 | **112** | 1514 / 1145 |
| nearpoint + `point()` stand-in | 8 | 51 | 28.6 / 31.6 / 28.9 | 26.3 | 35.2 | 301 / 482 |
| branchy `if`/`else`, `sin` on one side | 18 | 9.5 | 23.9 / 33.1 / 30.4 | 16.7 | 19.1 | 222 / 166 |

How to read the table:

- **Pointwise maths and memory-bound snippets.** The interpreter is level with the JIT and with hand-written C++. The page sweep dominates, and dispatching one instruction over 64-1024 lanes costs almost nothing.
- **Noise.** The JIT is 1.4x faster than both the interpreter and native C++. It inlines Natron's Perlin from bitcode and vectorises it for AVX2, whereas the interpreter calls the scalar builtin once per lane. A builtin written to work over a whole batch would close most of that gap without a JIT.
- **Branchy.** Native C++ takes the real branch, so it is fastest. The JIT if-converts the branch and the interpreter masks it, so both evaluate `sin` on every lane. That leaves them comparable (v3 interpreter 16.7, JIT 19.1).
- **nearpoint.** The interpreter is faster. Its gather batches independent lookups, while the JIT's per-element loop calls back into the host once per point.
- **Lane width.** 64 or 256 lanes are best; 1024 lanes spill past L1.

## Compile latency per keystroke-sized edit

Each edit appends one line, and the latency covers the front end plus the backend. Figures are median / max over 10 edits, in ms.

| Snippet | Interpreter | JIT, release LLVM |
|---|---|---|
| add | 0.03 / 0.04 | 23.9 / 27.1 |
| maths | 0.06 / 0.11 | 31.6 / 35.5 |
| noise | 0.05 / 0.07 | 68.9 / 79.7 |
| nearpoint | 0.05 / 0.07 | 11.0 / 14.7 |
| branchy | 0.07 / 0.12 | 34.9 / 37.6 |

- **JIT engine start:** 2.6 ms and about 4.5 MB RSS per engine.
- **Noise compile cost:** the noise snippet takes longer to compile because it parses and links the builtins bitcode on every compile. Caching the parsed module would trim that part, but optimisation and codegen would still take tens of ms.
- **Interpreter compile time:** a fraction of the time spent re-cooking the graph.

## Link and AppImage size cost of LLVM

| Item | Size |
|---|---|
| `/usr/lib64/libLLVM.so.21.1`, distribution release build, already stripped | 123 MiB |
| The same library, xz | 28.5 MiB |
| Image's Conan `libLLVM.so.21.1` (assertions): on disk / stripped / xz | 119 / 103 / 32 MiB |
| Static LLVM, only the ORC, X86, passes, bitreader and linker components | Not measurable here; the image has no static LLVM archives |
| Bench binary linking LLVM dynamically | 0.35 MiB |

Today's AppImages are 168-425 MB and do not bundle any LLVM. A shared libLLVM would add about 123 MB unpacked and about 29 MB compressed. A static build with hidden symbols and only the needed components would typically come in at a third to a half of that, but this is an estimate, not a measurement.

## Packaging risk

The risk is high for a bundled shared libLLVM.

- **SONAME clash with Mesa.** Both of the image's LLVMs and every current Mesa built on LLVM 21 use the SONAME `libLLVM.so.21.1` and the symbol version `LLVM_21.1`. The host's Mesa DRI driver is dlopen'd at GL init (see `tools/release/stage-bundle.sh`) and links the host's libLLVM.
- **What breaks.** If we bundle ours, whichever library loads first satisfies both Natron and Mesa. Ours has no AMDGPU target, which breaks radeonsi and llvmpipe. The host's may differ in ABI-breaking-checks mode or patch level, which breaks the JIT. The image already hit this clash: `undefined symbol: LLVMInitializeAMDGPUTargetInfo`, which the Dockerfile works around with patchelf.
- **This spike shows it too.** The bench's two builds can only pick between the two copies through `DT_RPATH`. Symbol-level ABI flags (`EnableABIBreakingChecks` versus `DisableABIBreakingChecks`) decide whether a binary resolves at all.
- **Safe option.** Link LLVM statically, with hidden visibility and `--exclude-libs,ALL`, so no LLVM symbol is exported. That needs an LLVM build or Conan package with static libraries, release mode and assertions off. That is new image and CI work, plus roughly 20-40 minutes added to a cold image build.
- **Other risks.** LLVM version bumps become a Natron release task. The JIT's codegen depends on each user's CPU (`detectHost`), so results can differ from one machine to another in the last ulp, unless we pin a target, as Natron's -O2 baseline does.

## Recommendation

Use the batched bytecode interpreter.

- **Runtime.** It is level with the JIT on 3 of 4 representative snippets and wins the memory-bound `nearpoint` case. The JIT's one clear win is noise, at 1.4x, and a batched `noise` builtin would close most of that gap.
- **Compile latency.** The interpreter compiles in about 0.1 ms, against 11-80 ms for the JIT on a release LLVM. Interactive editing never waits.
- **Packaging.** There is no packaging cost or risk. It builds with Natron's own compiler flags and gives bit-identical, CPU-independent results.
- **Headroom.** Building the interpreter for x86-64-v3 already gains up to 1.4x on the branchy snippet, and function multiversioning would deliver that without raising the baseline.
- **Keeping the JIT open.** The front end lowers to typed IR that either backend consumes. The JIT can be added later as an opt-in tier for hot kernels if it ever earns its packaging cost.

## Open for the user

- **Backend choice.** The interpreter is recommended. Picking the JIT would also commit to a static, hidden-visibility LLVM build in the image.
- **GPU-host run.** Should the same command be run on the GPU host before deciding? These numbers come from a 4-core N100 under build contention.
- **Interpreter follow-ups.** If the interpreter is chosen, these should be folded into M360.P3.T1: batch-form builtins (noise first), x86-64-v3 multiversioning of the execute loop, and compaction of active lanes for expensive branches.
