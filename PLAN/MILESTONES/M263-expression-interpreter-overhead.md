# M263 - Expression Interpreter Overhead

Cuts the fixed cost of every Python expression evaluation without changing what an expression means. Today each evaluation formats `"<fn>(time, view)\n"`, runs it through `PyRun_String` in `__main__`, and reads the result from the `ret` global (`KnobHelper::executeExpression`, Engine/Knob.cpp:3729-3800), and each expression function body begins with one `hasattr`-guarded assignment per sibling node (`KnobHelperPrivate::declarePythonVariables`, Engine/Knob.cpp:2812-2914), so a graph of N nodes pays O(N) per evaluation and O(N²) in total. That prelude has a guard because of upstream issue MrKepzie/Natron#1453 (commit c1baa0472): an unguarded assignment for a deleted or renamed sibling broke every expression in the group. Sibling-name resolution follows the decision recorded by M266 - Expression Namespace Spike.

## Scope

**In:**
- Calling the cached function object directly, with no source string and no `ret` global, for the per-frame evaluation path.
- Resolving bare sibling names without a per-sibling line per evaluation, by the mechanism M266 - Expression Namespace Spike chose: a group-owned native resolver behind a namespace shim, or referenced-names-only emission.
- An unreferenced deleted or renamed sibling never affects an expression.
- Holding the GIL once per batch of evaluations, only if the measurement in Phase 263.3 shows GIL handoff is a real share of the cost.
**Out:**
- Caching results (M264 - Expression Result Cache).
- Any native expression evaluation (M265 - Native Expression Evaluator). On the shim path the group-owned resolver is built here and M265 reuses it; on the fallback path M265 builds it.
- The explicit `app1.Group.Node` attribute path, `declareNodeVariableToPython` (Engine/Node.cpp:8468-8540) and the rename text rewriting (`replaceNodeNameInExpression`, Engine/Knob.cpp:3318): unchanged.
- Removing or changing the bare-name syntax. `Grade2.gain.get()` keeps working; dropping it for `thisGroup.X` or a string `node("X")` lookup was considered and set aside (Decisions).
- Names built dynamically (`eval`, `getattr`, string-built identifiers) are unsupported by the referenced-names path, as they are not dependency-tracked today either.
- `validateExpression`'s one-time compile-and-test run, which keeps using `ret` (it runs once per edit).
**Edge cases:**
- A sibling renamed, deleted or added after the function was defined resolves at call time, so the expression sees the current node or raises the same `NameError` as before.
- An unreferenced sibling deleted or renamed: the expression is unaffected (the #1453 case); a regression test pins it.
- A sibling named like a Python keyword: `declareNodeVariableToPython` already throws; keep the definition-time keyword check.
- Expressions that assign `ret` themselves (`hasRet`): the generated function still returns it, so the call path never reads the global.
- The dependency pass `parseListenersFromExpression` (Engine/Knob.cpp:2917-2979) runs the same prelude and must register the same dependencies after the change.
- Recursive evaluation (`EXPR_RECURSION_LEVEL`) and render threads: a batch scope holds the GIL only for one batch and never across a call into the engine that can block on another thread (the recursion class behind upstream issue 379).
**Assumptions:**
- `PythonGILLocker` (Engine/AppManager.cpp:3939) wraps `PyGILState_Ensure`, which nests, so a batch scope nests with the per-knob lockers in `evaluateExpression`.
- Unconfirmed: the 15% GIL-handoff threshold in M263.P3.T1 is a proposal; the user may set a different one.
- Unconfirmed: the embedded Python honours `__missing__` on a `dict`-subclass globals; M266 - Expression Namespace Spike checks it.

## Phase 263.1: Direct call

- [ ] M263.P1.T1 — Call the cached expression function object directly
  - files: Engine/Knob.cpp, Engine/Knob.h
  - approach: Add `KnobHelper::executeExpressionFunction(double time, ViewIdx view, int dimension, PyObject** ret, std::string* error)`. It looks the function up once per expression and keeps a strong `PyObject*` in the dimension's `Expr` (drop it when the expression is set, cleared or its node renamed, under `expressionMutex`; release it under the GIL). It calls `PyObject_CallFunction(fn, "(dI)", time, view)` and returns the result as a new reference. Errors go through the existing `catchErrors`. The `executeExpression(double time, ...)` overload (Engine/Knob.cpp:3729) calls it instead of building source. The string overload at :3760 stays for callers that run arbitrary scripts.
  - verify: existing expression tests pass (`Tests/GraphScalingExpressionDeps_Test.cpp` and the Python expression cases); a new gtest in Tests/KnobExpressionCall_Test.cpp sets an expression on a Double knob, evaluates it at three frames, renames the node and checks the value still resolves, then clears it and checks no stale function is called.
  - size: L

- [ ] M263.P1.T2 — Generate the expression function so it returns its value, with no `ret` on the call path
  - files: Engine/Knob.cpp
  - approach: In `validateExpression` (Engine/Knob.cpp:2982-3138) the generated function already ends `return ret`. For single-line expressions emit `return (<expr>)` directly instead of `ret = <expr>` then `return ret`; keep `ret` for `hasRetVariable` bodies, whose user code assigns it. The one-time validation run keeps reading `ret` from `__main__`.
  - verify: tests from M263.P1.T1 still pass; a gtest case covers a multi-line `ret =` expression and a single-line expression with a trailing comment.
  - size: M

## Phase 263.2: Sibling names (do the path M266 chose; strike the other with its reason)

- [ ] M263.P2.T1 — Add a group-owned native name resolver with a generation counter
  - files: Engine/NodeGroup.h, Engine/NodeGroup.cpp, Tests/NodeCollectionResolver_Test.cpp
  - approach: Shim path only. Add `NodeCollection::resolveScriptName(const std::string&)` returning the activated, non-multi-instance sibling with that script name (an O(1) hash lookup kept alongside the node list), and a `getNodeSetGeneration()` that every add, remove, activate, deactivate and rename increments. It reads the live graph; it keeps no per-expression state.
  - verify: gtest in Tests/NodeCollectionResolver_Test.cpp: add, remove, rename and deactivate each change what the resolver returns and bump the generation; a multi-instance child is not returned.
  - size: M

- [ ] M263.P2.T2 — Use the resolver as the expression functions' namespace and drop the per-sibling prelude
  - files: Engine/Knob.cpp, Engine/Knob.h, Engine/NodeGroup.cpp
  - approach: Shim path only. Define the namespace type in Python (a `dict` subclass whose `__missing__` calls the owning collection's `resolveScriptName`, then falls through to `__main__`'s dict) and keep one instance per collection. Cache each resolved node wrapper in it and clear that cache when `getNodeSetGeneration()` moves. Create each expression function with that namespace as its globals; remove the per-sibling lines from `declarePythonVariables` (Engine/Knob.cpp:2850-2866) and keep `app`, `thisGroup`, `thisNode`, `thisParam`, `random`, `randomInt`, `curve` and `dimension`. The listener pass `parseListenersFromExpression` evaluates against the same namespace and must register the same dependencies. Keep the keyword check at definition time.
  - verify: gtest in Tests/KnobExpressionSiblings_Test.cpp: a bare sibling reference evaluates; renaming the sibling makes the old name raise the same error text as before and the new one work; a sibling added after the expression was set resolves; deleting an unreferenced sibling leaves the expression valid; a helper defined in `__main__` is visible; listeners match the previous registration.
  - size: L

- [ ] M263.P2.T3 — Emit the sibling line only for identifiers the expression uses
  - files: Engine/Knob.cpp, Tests/KnobExpressionSiblings_Test.cpp
  - approach: Fallback path only (used when M266 - Expression Namespace Spike chose referenced-names-only). In `declarePythonVariables` extract the identifiers of the expression with `pythonCodeMask` (Engine/Knob.cpp:2584-2629) and emit the existing `if hasattr(...)` assignment only for those equal to a sibling's script name, looked up in a hash set built once per call. The parse for dependencies uses the same expression text.
  - verify: the gtest cases of M263.P2.T2 pass (the namespace-specific ones adapted); an expression referencing one of 1000 siblings emits one sibling line.
  - size: M

## Phase 263.3: One GIL per batch (measure first)

- [ ] M263.P3.T1 — Measure GIL handoff under parallel expression evaluation
  - files: tools/bench/BASELINE.md
  - approach: After P1 and P2, run the M262 - Expression Benchmark For Large Graphs `exprmixed` topology at N=1000 with `BENCH_RANGE` frames rendered in parallel, on the host with the most cores available, and sample thread states as in the profiling notes (`/proc` states, `tools/bench/node_profile.sh`). Record the share of render-thread time spent waiting on the GIL and the core count. Decide: implement P3.T2 only if that share is at least 15% (a proposal for the user to confirm); otherwise strike P3.T2 and record the numbers as the reason.
  - verify: BASELINE.md has the share, the core count and thread count, and `## Decisions` states the go or no-go.
  - size: M

- [ ] M263.P3.T2 — Add an ExpressionBatchScope that holds the GIL across a batch of evaluations
  - files: Engine/Knob.h, Engine/Knob.cpp, Engine/KnobImpl.h
  - approach: Only if P3.T1 says go. Add `class ExpressionBatchScope` (RAII, takes `PythonGILLocker` once, nests safely). Use it where one caller evaluates many knobs in a row: the per-frame value fetch that reads every knob of an effect for a render or metadata refresh. Find those call sites first (`getValueAtTime` loops in Engine/EffectInstance.cpp and Engine/Node.cpp) and wrap only those, never a scope that spans a call that waits on another thread. The per-knob lockers in `evaluateExpression` stay (they nest).
  - verify: the gtests from P1 and P2 pass; a gtest evaluating 100 expression knobs inside one scope returns the same values as outside one; the P3.T1 measurement re-run shows the GIL-wait share dropped.
  - size: M

## Phase 263.4: Measure

- [ ] M263.P4.T1 — Re-run the expression matrix and record the gain
  - files: tools/bench/BASELINE.md
  - approach: After one remote or local build of the milestone, run the M262 matrix and append the "after M263" rows next to the baseline. Run the GUI bench at the gate.
  - verify: BASELINE.md shows before and after for every topology and size; `exprchain` N=5000 cold time per evaluation is at least 5x below the baseline's, and the N=5000 to N=100 per-evaluation time ratio is under 3.
  - size: S

**Verification gate:** all expression gtests pass; every expression in the existing `.ntp` comps and the Python expression tests evaluates to the same value as before; the #1453 case (unreferenced sibling deleted) stays fixed; the GUI bench (`tools/bench/gui/run.sh --xvfb`, `compare.py`) shows no regression and is recorded in `## Decisions`; the M262 matrix shows at least 5x lower cold per-evaluation time at `exprchain` N=5000 and a N=5000 to N=100 ratio under 3.

## Decisions

- 2026-10-27 — Keep bare sibling names (user): `Grade2.gain.get()` is the most readable syntax and stays unless keeping it has a real cost. Dropping it for `thisGroup.X` or a string `node("X")` was considered the cleaner syntax and set aside because a Python micro-test (3.11.2) showed the fixed cost of the alternative is small.
- 2026-10-27 — Shim gated on a spike (user): the namespace shim over a group-owned native resolver is preferred; M266 - Expression Namespace Spike measures it against referenced-names-only on the embedded interpreter, and M263's Phase 263.2 implements the winner. Micro-test on Python 3.11.2: shim about 1120 ns vs 284 ns plain per evaluation with 4 global names (the non-exact-dict cost, about 200 ns per lookup), current prelude 0.59 µs at N=10, 6.4 µs at N=100, 118 µs at N=1000.
- 2026-10-27 — GIL batching is measured and droppable (user): PyGILState_Ensure nests and is cheap uncontended, so P3.T2 only ships if P3.T1 shows GIL handoff is a real share on a many-core host.
- 2026-10-27 — Gate (user): at least 5x lower cold per-evaluation time at `exprchain` N=5000 and a N=5000 to N=100 ratio under 3.
