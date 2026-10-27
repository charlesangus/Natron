# M263 - Expression Interpreter Overhead

Cuts the fixed cost of every Python expression evaluation without changing what an expression means. Today each evaluation formats `"<fn>(time, view)\n"`, runs it through `PyRun_String` in `__main__`, and reads the result from the `ret` global (`KnobHelper::executeExpression`, Engine/Knob.cpp:3729-3800), and each expression function body begins with one `hasattr`-guarded assignment per sibling node (`KnobHelperPrivate::declarePythonVariables`, Engine/Knob.cpp:2815-2915), so a graph of N nodes pays O(N) per evaluation and O(N²) in total.

## Scope

**In:**
- Calling the cached function object directly, with no source string and no `ret` global, for the per-frame evaluation path.
- Resolving sibling nodes lazily instead of one `hasattr` per node per call.
- Holding the GIL once per batch of evaluations rather than once per knob.
**Out:**
- Caching results (M264 - Expression Result Cache).
- Any native evaluation (M265 - Native Expression Evaluator).
- Changing expression syntax or what names an expression can see. Every expression that evaluated before evaluates to the same value.
- `validateExpression`'s one-time compile-and-test run, which keeps using `ret` (it runs once per edit).
**Edge cases:**
- A sibling renamed, deleted or added after the function was defined: lookups resolve at call time, so the expression sees the current node, or raises the same `NameError` as before.
- A sibling named like a Python keyword or builtin: `declarePythonVariables` throws today for keywords; keep that check at definition time. A sibling shadowing a builtin keeps shadowing it.
- Expressions that assign `ret` themselves (`hasRet`): the generated function still returns it, so the call path never reads the global.
- Recursive evaluation (`EXPR_RECURSION_LEVEL`) and render threads: the batch scope holds the GIL only for the extent of one batch and never across a call into the engine that can block on another thread.
**Assumptions:**
- `PythonGILLocker` is re-entrant (it wraps `PyGILState_Ensure`), so a batch scope nests with the per-knob lockers still present in `evaluateExpression`.
- Python's `__getattr__` is not available on module globals as a hook for bare names, so the lazy prelude is a dict subclass or a `__builtins__` fallback consulted on a missing name. The first task settles which and records it as a decision.

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

## Phase 263.2: Lazy siblings

- [ ] M263.P2.T1 — Resolve sibling nodes on demand instead of one hasattr per node
  - files: Engine/Knob.cpp, Engine/PythonSupport.cpp, Engine/PythonSupport.h
  - approach: Stop emitting the per-sibling `if hasattr(...): name = app.group.name` lines in `declarePythonVariables`. Give each expression function a globals dict whose missing-name hook (a dict subclass with `__missing__`, or a function defined with `exec` against that dict — settle which in a first step and record it) looks the name up among the owning group's activated, non-multi-instance sibling nodes and returns `app.<fullyQualifiedName>`. Keep the keyword check at definition time and the `app`, `thisNode`, `thisGroup`, `thisParam` definitions. The hook caches nothing: node renames and deletions must show up on the next call.
  - verify: gtest in Tests/KnobExpressionSiblings_Test.cpp: an expression referencing a sibling by bare name evaluates; after renaming the sibling the old name raises the same error text as before and the new name works; a sibling added after the expression was set resolves. `tools/bench/graph_bench.py` with `BENCH_TOPO=exprchain` at N=1000 no longer scales quadratically in cold time per evaluation (ratio of N=1000 to N=100 per-evaluation time under 3).
  - size: L

## Phase 263.3: One GIL per batch

- [ ] M263.P3.T1 — Add an ExpressionBatchScope that holds the GIL across a batch of evaluations
  - files: Engine/Knob.h, Engine/Knob.cpp, Engine/KnobImpl.h
  - approach: Add `class ExpressionBatchScope` (RAII, takes `PythonGILLocker` once, nests safely). Use it where one caller evaluates many knobs in a row: the per-frame value fetch that reads every knob of an effect for a render or metadata refresh. Find those call sites first (`getValueAtTime` loops in Engine/EffectInstance.cpp and Engine/Node.cpp) and wrap only those, never a scope that spans a call that waits on another thread. The per-knob lockers in `evaluateExpression` stay (they nest).
  - verify: the gtests from M263.P1.T1 and M263.P2.T1 pass; a gtest evaluating 100 expression knobs inside one scope returns the same values as outside one; the `exprmixed` N=1000 cold time per evaluation drops against the M262 - Expression Benchmark For Large Graphs baseline.
  - size: M

## Phase 263.4: Measure

- [ ] M263.P4.T1 — Re-run the expression matrix and record the gain
  - files: tools/bench/BASELINE.md
  - approach: After one remote or local build of the milestone, run the M262 matrix and append the "after M263" rows next to the baseline. Run the GUI bench at the gate.
  - verify: BASELINE.md shows before and after for every topology and size, and `exprchain` N=5000 cold time per evaluation is below the baseline's by at least 5x, or the milestone's `## Decisions` explains why not.
  - size: S

**Verification gate:** all expression gtests pass; every expression in the existing `.ntp` comps and the Python expression tests evaluates to the same value as before; the GUI bench (`tools/bench/gui/run.sh --xvfb`, `compare.py`) shows no regression and is recorded in `## Decisions`; the M262 matrix shows the gain recorded in M263.P4.T1.
