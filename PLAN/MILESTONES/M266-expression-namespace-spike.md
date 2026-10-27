# M266 - Expression Namespace Spike

Decides how M263 - Expression Interpreter Overhead resolves bare sibling-node names in expressions. Today every expression function starts with one `hasattr`-guarded assignment per sibling (`KnobHelperPrivate::declarePythonVariables`, Engine/Knob.cpp:2812-2914), run on every evaluation, so cost is O(N) per evaluation and O(N²) over a group. Two replacements are candidates: a namespace shim (a `dict` subclass with `__missing__`, backed by a group-owned native resolver, used as the expression functions' globals) and referenced-names-only emission (the prelude kept, but only for identifiers the expression uses). A micro-test on Python 3.11.2 outside Natron showed the shim costs about 200 ns per global lookup of any name (the non-exact-dict penalty, not `__missing__`), crosses the current prelude near 20 siblings, and is about 100x faster at 1000. This spike measures both on the embedded interpreter and settles it. Nothing it writes ships.

## Scope

**In:**
- A throwaway gtest that runs both candidates on the embedded Python for synthetic groups of 10, 100, 1000 and 5000 siblings.
- A semantics checklist run against each candidate.
- A recorded decision in `## Decisions`, which M263 - Expression Interpreter Overhead implements.
**Out:**
- Any change under Engine/ that ships; the spike code lives only in its own test file.
- The explicit `app1.Group.Node` attribute path and `declareNodeVariableToPython` (Engine/Node.cpp:8468-8540): unchanged by every candidate.
- Dynamically built names (`eval`, `getattr`, string-built identifiers): unsupported by the referenced-names candidate and left as they are.
**Edge cases:**
- Semantics checklist, each answered yes or no per candidate: a helper function defined in `__main__` is visible from an expression; builtins (`sin` via `math` imports in `__main__`, `max`, `abs`) resolve; an unknown name raises `NameError` with the same text as today; a sibling renamed or deleted between two calls is not found on the second call and an unrelated deleted sibling does not affect the expression (upstream issue MrKepzie/Natron#1453); the listener pass in `parseListenersFromExpression` (Engine/Knob.cpp:2917-2979) registers the same dependencies.
**Assumptions:**
- The embedded Python is 3.11 or later and honours `__missing__` on a `dict`-subclass globals for `LOAD_GLOBAL`. Task 1 checks it.

## Phase 266.1: Measure and decide

- [ ] M266.P1.T1 — Spike both candidates in a throwaway gtest on the embedded interpreter
  - files: Tests/ExpressionNamespaceSpike_Test.cpp, Tests/CMakeLists.txt
  - approach: Follow the harness in Tests/GraphScalingExpressionDeps_Test.cpp for starting the interpreter. In Python, build for each N in 10, 100, 1000, 5000 a fake app object with N sibling attributes. Define three functions that return `S0.gain * sin(frame) + max(1, 2) + helper()`: (a) the current prelude form with N guarded assignments, (b) the shim form (a `dict` subclass whose `__missing__` looks the name up in a Python dict standing in for the group, used as the function's globals, with `helper`, `sin` and `__builtins__` copied in), (c) referenced-names-only (one guarded assignment, `S0`). Time each with `time.perf_counter_ns` over enough iterations for stable medians, print a table and print the Python version. Record the six semantics checks from the Scope as pass or fail per form.
  - verify: the test builds and prints the timing table, the Python version and a pass or fail for every check and form.
  - size: M

- [ ] M266.P1.T2 — Record the decision and the numbers
  - files: .plan/PLAN/MILESTONES/M266-expression-namespace-spike.md
  - approach: Paste the table into `## Decisions`. Apply the rule: keep the shim if it is no slower than referenced-names-only at N=5000 and within 1 µs of it at N=10, and passes every check that referenced-names-only passes; otherwise referenced-names-only is the Python-path mechanism and the native resolver waits for M265 - Native Expression Evaluator. Name the chosen form and cite the rule. Delete the spike source file and its CMake entry in the same change.
  - verify: `## Decisions` names the chosen form with its numbers; the spike source file is gone.
  - size: S

**Verification gate:** `## Decisions` holds the measured table, the checklist results and the chosen form, and M263 - Expression Interpreter Overhead's Phase 263.2 matches it (the matching task is kept, the other struck through).
