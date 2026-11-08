# M360 - Geometry Kernel Language

Fourth member of F15 - Native Geometry. A VEX-like, per-element snippet language for native
geometry and its wrangle node, Houdini's Attribute Wrangle. A snippet is compiled once and run
over one element class in page-sized batches on the thread pool. The runtime gathers bound
attributes from the pages and scatters writes back, so scripts never see pages or holes, and
only written pages detach. A spike settles first whether it runs as a batched bytecode
interpreter or through an LLVM JIT.
Shared design: M54 - Native Geometry Data Model's intro and Scope, `PLAN/DECISIONS/2026-11-08-native-geometry-follows-sops.md`.

Member of F15 - Native Geometry: branches off and merges into `feature/f15-native-geometry` (PLAN-FORMAT §9b); the Feature's UAT covers it.

## Scope

**In:**
- A spike comparing a batched bytecode interpreter with an LLVM JIT, reported for the user's choice.
- A VEX-compatible subset of syntax: typed declarations, `@attr` with `f@`/`i@`/`v@`/`p@`/`s@` prefixes, `@ptnum`/`@primnum`/`@vtxnum`/`@numpt`/`@Frame`/`@Time`, and VEX builtin names and signatures for the subset implemented. Unsupported functions give a clear compile error.
- Second-input reads (`point(1, "P", i)`, `@opinput1_P`), `nearpoint`/`nearpoints` backed by a cached point tree, group functions, and topology functions (`addpoint`, `addprim`, `addvertex`, `removepoint`, `removeprim`, `setpointgroup`, …) queued per batch and applied after the run, as Houdini does.
- `ch`/`chi`/`chf`/`chv`/`chs`/`chramp` reading user knobs on the node, plus a button that creates missing knobs from the snippet.
- A front end and runner that are free of `GeoDetail` types behind a binding interface, so another context can plug in later.

**Out:**
- Image, deep and volume wrangles in the same language (none yet). The core stays reusable for them.
- Moving DeepExpression onto the new language (none).
- The name "VEX" anywhere in the product. It is described as VEX-like at most, and SideFX documentation text, examples and headers are never copied; the docs are written fresh.
- The Python wrangle (M359 - Geometry Operators).

**Edge cases:**
- Out-of-range reads (`point()` past the end, a missing attribute) return zero, as in VEX; maths follows IEEE.
- Topology edits from many threads: queued per batch and applied in element order after the run, so the result does not depend on thread count.
- A compile error: node error with line and column, nothing rendered, render thread unharmed.
- A snippet using an unsupported VEX function: a compile error naming the function.

**Assumptions:**
- Unconfirmed: the kernel node is named `GeoWrangle` (and the Python node `GeoPython`), following Houdini.
- Clean-room reimplementation of a language's syntax and function names from public behaviour is legally sound (SAS v. World Programming; Google v. Oracle; OpenVDB AX as industry precedent). Not legal advice; trademark and documentation copying are avoided per Out.

Requires M54 - Native Geometry Data Model. Nodes register through their own `.reg.inc` under `PLUGIN_GROUP_3D`.

## Phase 360.1: Execution spike

- [ ] M360.P1.T1 — Spike: batched bytecode interpreter vs LLVM JIT
  - files: `tools/spikes/geo-kernel/` (throwaway prototypes, not built by default), `PLAN/DESIGN/<date>-geo-kernel-execution-spike.md` (report, on the plan branch)
  - approach: prototype both backends on one tiny shared front end (expressions, `@P`/`@ptnum`, `sin`, `noise`, one `if`). The interpreter runs a typed register bytecode over a whole page per instruction. The JIT emits LLVM IR per snippet through the system LLVM 21 in the dev image. Run 3–4 representative snippets (point-wise maths, a noise displace, a `nearpoint` lookup stand-in, a branchy snippet) over 10M points on the GPU host, one command for both. Report runtime per snippet, compile latency per keystroke-sized edit, the link and AppImage size cost of LLVM, and the packaging risk. Recommend one, then park the milestone with `Needs: response` for the user's choice (user, 2026-11-08). Elaborate or rewrite M360.P3.T1 after the choice.
  - verify: the report exists with numbers for both backends on every snippet, and the user's choice is recorded in `## Decisions`.
  - size: L

## Phase 360.2: Front end

- [ ] M360.P2.T1 — VEX-like front end with a context-neutral binding interface
  - files: `Engine/Kernel/KernelLexer.h`/`.cpp`, `Engine/Kernel/KernelParser.h`/`.cpp`, `Engine/Kernel/KernelTypes.h`, `Engine/Kernel/KernelBinding.h`, `Tests/KernelParser_Test.cpp`
  - approach: a lexer and parser for the VEX-compatible subset in Scope, growing from `Engine/Nodes/Deep/DeepExpressionEvaluator`'s precedence table but typed. Types are int, float, vector2/vector/vector4, matrix3/matrix and string, with VEX's implicit conversions. Statements: declarations, assignment and compound assignment, `if`/`else`, `for`, `foreach`, `while`, `return`, user functions. `@name` bindings with type prefixes or inferred for known names (`@P`, `@N`, `@Cd`, `@uv`, `@pscale`, `@orient`, `@id`). Name and type resolution goes through `KernelBinding`, an interface a context implements (attribute lookup by class, builtin variables, functions it provides), so nothing here sees `GeoDetail`. It produces a typed IR the chosen backend consumes, and errors carry line and column. Builtin names and signatures follow VEX's public documentation; the docs are written fresh.
  - verify: unit tests — a corpus of valid snippets type-checks to the expected IR shape; type errors, unknown names and unsupported VEX functions report the right line and column; `@P.y += 1` binds `P` as a vector write; `Engine/Kernel/` includes no `GeoDetail` header (grep-enforced).
  - size: L

## Phase 360.3: Runner and geometry builtins

- [ ] M360.P3.T1 — Batched, multithreaded kernel runner over pages
  - files: `Engine/Kernel/KernelRunner.h`/`.cpp`, `Engine/Geo/GeoKernelBinding.h`/`.cpp`, `Tests/GeoKernelRunner_Test.cpp`
  - approach: written for the interpreter; rewrite it before it runs if M360.P1.T1's choice is the JIT. How VEX hides pages. Run the compiled IR over one element class (points, primitives, vertices or detail), through `GeoKernelBinding`, the geometry implementation of `KernelBinding`, in batches of a page. The runner gathers bound attributes into batch registers, runs the program across the batch, and scatters writes back, so only written pages detach. Batches run on the host thread pool, skip tombstones, and present `@ptnum` as the dense number. Writes to an attribute that doesn't exist create it, typed by the binding. Abort is checked between batches.
  - verify: unit tests — `@P.y += 1` over 10M points matches a scalar reference; pages not written stay shared; results are identical across thread counts; holes are skipped and `@ptnum` stays dense; an abort stops within one batch per thread.
  - size: L

- [ ] M360.P3.T2 — Topology, group and multi-input builtins
  - files: `Engine/Geo/GeoKernelBinding.cpp`, `Engine/Geo/GeoKernelTopology.h`/`.cpp`, `Engine/Geo/GeoPointTree.h`/`.cpp`, `Tests/GeoKernelTopology_Test.cpp`
  - approach: geometry builtins on top of M360.P3.T1. Second-input reads: `point(1, "P", i)`, `prim`, `vertex`, `detail`, `@opinput1_<name>`, `npoints(1)`. `nearpoint`/`nearpoints` use a point tree built once per input detail and cached on it. Group functions: `inpointgroup`, `setpointgroup`, `setprimgroup`, `inprimgroup`. Topology functions: `addpoint`, `addprim`, `addvertex`, `removepoint`, `removeprim`, `setprimvertex`. Their edits are queued per batch and applied after the run in element order, with new elements numbered as Houdini numbers them, so results don't depend on thread count. Removal tombstones (M54.P1.T5).
  - verify: unit tests — `addpoint` per point doubles the count identically at 1 and 16 threads; `removepoint` of odd points leaves the even ones in order; `nearpoint` on a second input matches brute force; group writes show up in the output groups.
  - size: L

## Phase 360.4: The wrangle node

- [ ] M360.P4.T1 — The kernel wrangle node
  - files: `Engine/Nodes/Geometry/GeoWrangle.h`/`.cpp`/`.reg.inc`, `Engine/Nodes/Geometry/docs/GeoWrangle.md`, `Tests/GeoWrangle_Test.cpp`
  - approach: the Attribute Wrangle (node name: Scope assumption). Knobs: snippet (multi-line `KnobString`), "Run Over" (points, primitives, vertices, detail), optional group restriction. It compiles once per snippet change and runs through M360.P3.T1. A compile error becomes a node error with line and column. The doc page lists the language, bindings and builtins.
  - verify: unit test — a snippet doubling `@P.z` doubles Z; running over primitives with a group touches only that group; a compile error reports line and column on the node without aborting the render thread.
  - size: M

- [ ] M360.P4.T2 — `ch()` functions and the create-parameters button
  - files: `Engine/Nodes/Geometry/GeoWrangle.cpp`, `Engine/Geo/GeoKernelBinding.cpp`, `Tests/GeoWrangleParams_Test.cpp`
  - approach: `ch`/`chi`/`chf`/`chv`/`chs`/`chramp` read user knobs on the node at the render time, resolved by name at compile time, so the node's frame-varying hash follows the knobs (an animated knob makes the node time-varying). A button scans the snippet for `ch*` calls and adds the missing user knobs with types from the call (`chf` → float, `chv` → 3D double, `chramp` → curve), through Natron's existing user-knob mechanism, so they animate, link and serialise like any knob. Knobs already present are left alone.
  - verify: unit test — `@P.y += chf("amp")` follows an animated `amp` knob across frames; the button creates `amp` as a float knob and a second press adds nothing; a renamed knob gives a compile error naming it.
  - size: M

**Verification gate:** M360.P1.T1's report exists and the user's backend choice is recorded; CI green with every Phase 360.2–360.4 test; a 10M-point `@P.y += 1` run is recorded on the GPU host in the PR body; the pre-existing ctest suite is green.

## Decisions

- 2026-11-08 — Split out of M54 - SOPs-Style Geometry (user, /cat-plan); tasks renumbered from M54.P6.*. Both a Python wrangle and this native kernel ship in F15 - Native Geometry (user, M54 grill).
- 2026-11-08 — **VEX-compatible syntax (user, /cat-plan grill)** after checking the IP question: syntax and function names are functional, as an API is; avoid the VEX trademark and copying SideFX docs.
- 2026-11-08 — **A spike decides interpreter vs LLVM JIT, and the user chooses from its report (user).** OpenVDB AX was considered and not adopted: it is built around VDB point grids and AX's builtins.
- 2026-11-08 — **Topology functions, second-input reads and `nearpoint` are in (user);** without them many common wrangles don't work.
- 2026-11-08 — **`ch()` functions plus a create-parameters button (user).**
- 2026-11-08 — **Geometry only, with a reusable core (user):** the front end and runner sit behind a binding interface with no `GeoDetail` types, so image, deep or volume contexts can plug in later.
