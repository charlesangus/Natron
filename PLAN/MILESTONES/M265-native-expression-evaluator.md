# M265 - Native Expression Evaluator

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A small native evaluator for the common expression forms, so they run without the GIL and without Python: arithmetic, `frame`, `sin` and the other pure maths functions, `clamp`, `noise`, and references to other knobs. It is invisible to the user: the expression field, the Python API, saved projects and error behaviour are unchanged. Whatever the native parser does not accept falls back to the existing Python path (`KnobHelper::executeExpression`, as made faster by M263 - Expression Interpreter Overhead), so any expression that exceeds the subset behaves exactly as it does today. The parse is a pure function of the expression text and runs at set time; a parsed expression compiles to a small bytecode or closure tree evaluated against a knob-reference resolver that reads values without taking the GIL.

Blocked on: M263 - Expression Interpreter Overhead and M264 - Expression Result Cache landing, and the user's go-ahead after reading the M264 - Expression Result Cache benchmark decision. This milestone only runs if the cold `exprmixed` N=5000 expression time still misses the threshold M262 - Expression Benchmark For Large Graphs recorded. The measured per-kind split also says which forms are worth covering first.

Acceptance sketch:
- On the M262 - Expression Benchmark For Large Graphs matrix, cold `exprmixed` N=5000 meets the recorded threshold.
- A property test generates expressions in the subset and shows native and Python results agree (bit-equal for arithmetic, within a few ulps for `sin`, `noise` matches the Python `noise` exactly).
- Any expression outside the subset, and any parse error, takes the Python path with the same value and the same error text as before; nothing in the UI shows which path ran.
- Native evaluation holds no GIL and runs on render threads in parallel.

## Scope

**In:**
- A parser for the subset, evaluated natively; fallback to Python for everything else, decided per expression at set time.
**Out:**
- Any user-visible switch, label or syntax change.
- Multi-statement `ret` bodies and anything that calls Python objects other than knob reads: always Python.
**Assumptions:**
- Knob reads need the owning value locks only, so a native resolver can run without the GIL.
- Name resolution reuses the group-owned native resolver if M263 - Expression Interpreter Overhead built one (the shim path chosen by M266 - Expression Namespace Spike); otherwise this milestone builds it.
