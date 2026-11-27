# M638 - Self-Dimension Expression Listeners

An expression that reads another dimension of its own knob never updates: `size.h = thisNode.size.getValue(0)` keeps its first result when `size.w` changes (from F16 - User Knob Editor's gate; pre-existing on main). Self-reads register no listener. `Engine/PyParameter.cpp:1024` returns early when the caller is the expression knob, and `Engine/Knob.cpp:3310` (`registerExpressionReads`) skips `source == thisKnob`. The refresh code at `Engine/Knob.cpp:6149` already handles a knob that listens to itself, so a per-dimension self-listener may fit without a new loop. Register cross-dimension self-reads, and keep refusing a dimension that reads itself.

## Scope

**In:**
- Cross-dimension reads within one knob, for Python expressions (and exprtk if it shares `registerExpressionReads`).

**Out:**
- Cycle handling between different knobs, which already works.

**Edge cases:**
- **Dimension reads itself (`size.w = size.w + 1`):** still refused as a cycle.
- **w reads h and h reads w:** refused as a cycle, with the same message as a two-knob cycle.
- **Multiview:** a read of another view of the same dimension counts as cross-dimension.

## Phase 638.1: Register self-reads

- [ ] M638.P1.T1 — Register cross-dimension self-reads as listeners and refresh dependents on change
  - files: `Engine/PyParameter.cpp` (`_addAsDependencyOf` ~:1024), `Engine/Knob.cpp` (`registerExpressionReads` ~:3310, `refreshListenersAfterValueChange`, ~:6149); expression tests (`Tests/RFVExpression_Test.cpp` or the knob expression test file; find the one covering `getValue` dependencies)
  - approach: let a self-read register when the read dimension/view differs from the expression's own, storing it per dimension. In `refreshListenersAfterValueChange`, when a dimension of a knob changes, re-evaluate the knob's other dimensions that read it, guarded against re-entry. Keep the early return for a same-dimension read and report it as a cycle.
  - verify: new tests: h follows w through three changes; w=w+1 is refused; w↔h is refused; existing expression tests green
  - size: M

**Verification gate:** full build and ctest green; GUI bench (knob-expression rule) shows no regression.
