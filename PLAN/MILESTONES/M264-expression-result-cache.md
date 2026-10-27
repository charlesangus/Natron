# M264 - Expression Result Cache

Makes an expression's evaluated value cost nothing after its first evaluation whenever its inputs have not changed. Knobs already keep a per-dimension result map keyed on time alone (`_exprRes`, Engine/Knob.h:2519, read and filled in `Knob<T>::getValueFromExpression`, Engine/KnobImpl.h:471-520, cleared by `clearExpressionsResults`, Engine/Knob.h:2414, guarded by a generation counter). It ignores the view, takes the knob's value mutex on every hit, and a time-independent expression is evaluated once per frame instead of once. M100 - Content Node Hashing added knob content hashes and `getContentHashDependencies` (Engine/Knob.cpp:5788), which say whether a knob's value can change without its own edit.

## Scope

**In:**
- Keying the cache on (dimension, time, view).
- Classifying an expression as time-independent and caching one value for every frame.
- Making a hit cheap under many render threads.
- Invalidation that stays exactly as correct as today: any change to a knob an expression reads clears the result.
**Out:**
- Making evaluation itself faster (M263 - Expression Interpreter Overhead).
- Native evaluation (M265 - Native Expression Evaluator).
- Caching across project save and load, or across processes.
**Edge cases:**
- An expression that reads `frame`, `time`, `thisNode.<metadata>`, another knob animated in time, or calls `random()` or a Python function with side effects: classified time-dependent or uncacheable, never promoted.
- Classification errs towards "dependent": the analysis scans the expression text with `pythonCodeMask` (Engine/Knob.cpp:2584-2629) for any identifier outside a small allow-list of pure names (numeric literals, arithmetic, `abs`, `min`, `max`, `pow`, `sqrt`, `sin`, `cos`) and bare references to non-animated knobs. Anything else is dependent.
- Multi-view projects: a value cached for one view is never returned for another.
- A result computed while a clear lands is dropped (the existing generation guard) and stays dropped.
**Assumptions:**
- `KnobHelperPrivate::parseListenersFromExpression` (Engine/Knob.cpp:2917) already registers every knob an expression reads as a listener, so the existing clear-on-change path reaches every dependent.

## Phase 264.1: Key and classify

- [ ] M264.P1.T1 — Key the expression result cache on view as well as time
  - files: Engine/Knob.h, Engine/KnobImpl.h
  - approach: Change `FrameValueMap` use for `_exprRes` to a map keyed on `(ViewIdx, double)` (or one map per view) in the `Knob<T>` template, in both `getValueFromExpression` (Engine/KnobImpl.h:471) and its `_pod` and string variants (:527, :579). `clearExpressionsResults` clears every view. The accessor at Engine/Knob.h:2364 that copies the map keeps its meaning for callers.
  - verify: gtest in Tests/KnobExpressionCache_Test.cpp: an expression using `view` returns a different value per view and each is cached separately; a clear drops both.
  - size: M

- [ ] M264.P1.T2 — Classify expressions as time-independent
  - files: Engine/Knob.cpp, Engine/Knob.h
  - approach: When an expression is set (`setExpressionInternal`, Engine/Knob.cpp:3234), compute `bool timeIndependent` for the dimension from the expression text and the knobs it reads, conservatively as in this milestone's edge cases, and store it in the dimension's `Expr` beside `hasRet`. It is recomputed when a listener's animation state changes (the existing listener path), and is false for `hasRet` bodies with more than one statement. Expose it as `KnobHelper::isExpressionTimeIndependent(int dimension)`.
  - verify: gtest cases in Tests/KnobExpressionCache_Test.cpp: `2*3` and `thisNode.other.get()*2` (other not animated) are independent; `frame*2`, `thisNode.other.get()` with `other` animated, `random()` and `thisNode.getMetadata(...)` are dependent; animating `other` flips the second.
  - size: L

## Phase 264.2: Use it

- [ ] M264.P2.T1 — Serve a time-independent expression from one cached value
  - files: Engine/KnobImpl.h
  - approach: In `getValueFromExpression` and its variants, when `isExpressionTimeIndependent(dimension)` holds, look up and store the result under a fixed time key (for example `0.`) so every frame hits the same entry. The generation guard stays.
  - verify: gtest: a constant expression is evaluated exactly once across 100 frames (count Python calls through a test hook or a Python-side counter in a sibling expression); editing a knob it reads makes the next call evaluate again.
  - size: M

- [ ] M264.P2.T2 — Make a cache hit lock-light
  - files: Engine/Knob.h, Engine/KnobImpl.h
  - approach: Replace the `_valueMutex` lock on the hit path with a shared (reader) lock on a dedicated `_exprResMutex` so concurrent render threads reading cached expression values do not serialise. Writers (insert, clear) take it exclusively. `_valueMutex` is no longer taken by the expression cache at all.
  - verify: existing knob and expression tests pass; a gtest reads one cached value from 8 threads 100000 times each and every read returns the expected value while another thread alternately clears and re-fills it.
  - size: M

## Phase 264.3: Measure

- [ ] M264.P3.T1 — Re-run the expression matrix and record warm and cold gains
  - files: tools/bench/BASELINE.md
  - approach: After one build, run the M262 - Expression Benchmark For Large Graphs matrix and append the "after M264" rows. Compare the `exprmixed` N=5000 cold first-frame expression time with the threshold recorded in M262's Decisions and write the comparison, plus the per-kind split, into this milestone's `## Decisions` so the user can decide on M265 - Native Expression Evaluator. Run the GUI bench.
  - verify: BASELINE.md has before and after rows for every topology and size; the Decisions entry states the figure, the threshold and whether the threshold is met.
  - size: S

**Verification gate:** all expression and knob gtests pass; evaluated values for every existing comp and Python expression test are unchanged; a multi-view expression returns per-view values; the GUI bench (`tools/bench/gui/run.sh --xvfb`, `compare.py`) shows no regression and is recorded in `## Decisions`; M264.P3.T1's figures are recorded.
