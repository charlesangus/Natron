# Geometry kernel execution spike

Compares two ways of running a per-point snippet over paged geometry: a batched bytecode
interpreter and an LLVM JIT. Both consume one tiny typed front end. It stands alone like
`tools/spikes/noise`, and nothing here is linked into Natron.

| File | What it is |
|---|---|
| `frontend.cpp` | Lexer, parser and type checker for a VEX-like subset: `int`/`float`/`vector` locals, `@P`/`@N`/`@Cd` (with `v@`/`p@` prefixes), `@ptnum`/`@numpt`/`@Time`, arithmetic, comparisons, `&&`/`||`/`!`, `if`/`else`, compound assignment, `.x/.y/.z`, and `sin cos sqrt abs floor min max length dot set noise nearpoint point`. Errors carry line and column. |
| `interp.cpp` | The interpreter. A typed register bytecode where each register holds one value per lane and each instruction runs over 64, 256 or 1024 lanes. Constants are hoisted into a per-thread prologue, branches run both sides under lane masks and skip a side whose mask is empty. Bound attributes are gathered from the page into registers (AoS to SoA) and scattered back. |
| `jit.cpp` | The JIT. Emits one LLVM IR function per snippet, a plain per-element loop over the page with attributes read from and written to the AoS page directly. Runs the O2 pipeline for the host CPU (`detectHost`) and compiles through ORC `LLJIT`. `noise` is linked in as bitcode (`builtins_bc.cpp`) so it inlines; `nearpoint` calls back into the host. |
| `builtins.h` | `noise` (Natron's `libs/noise` 4D Perlin at w = 0) and the `nearpoint` stand-in (a 64^3 grid-cell lookup plus a random gather, standing in for a tree query). |
| `bench.cpp` | Builds a paged detail (1024-element pages, AoS float3 `P` and `N`, `Cd` absent until written), runs every snippet through a hand-written C++ reference, the interpreter at each lane width and the JIT, on a thread pool over pages, and checks every output float bit for bit against the reference. |

Snippets: `add` (`@P.y += 1`), `maths` (length, sin, `@ptnum`), `noise` (a noise displace along
`@N`), `nearpoint` (`nearpoint` and `point()` on input 1, write a new `@Cd`), `branchy` (an
`if`/`else` on `@P.y` and `@ptnum % 3`, `sin` on one side).

Timings: `e2e` allocates every written page fresh, as a copy-on-write detach does; `warm` writes
into preallocated, already-touched pages, so it is the kernel plus batching alone. `compile` is
the first compile of each backend; the latency table times 20 one-line edits per snippet through
front end and backend.

## Build and run

Inside the natron-dev image, which has LLVM 21 (headers and an assertions build under
`/usr/local`, plus the distribution's release `libLLVM` in `/usr/lib64`) and clang for the
bitcode. The container is single-tenant: run this only when no other build is running.

    tools/spikes/geo-kernel/run.sh                      # 10M points, every snippet
    tools/spikes/geo-kernel/run.sh --points 2000000 --reps 3

From the host, without touching a running natron-dev container:

    docker run --rm --user "$(id -u):$(id -g)" --entrypoint bash \
        -v "$PWD:$PWD" -w "$PWD" natron-dev:<tag> tools/spikes/geo-kernel/run.sh

`run.sh` builds into `build/spikes/geo-kernel`, runs `gk_bench_rel` (JIT on the release
`libLLVM`; `gk_bench` uses the assertions build), then `gk_bench_v3` (interpreter and reference
built for x86-64-v3, no JIT), then prints libLLVM's size and SONAME. `gk_bench --help` lists
the options (`--threads`, `--lanes`, `--snippet`, `--jit-opt`, `--edits`, `--no-jit`).
`GK_DUMP_IR=1` prints each optimised JIT module to stderr.
