# M505 - Deleted Knob Listener Dependencies

`KnobHelper::deleteKnob` (`Engine/Knob.cpp:907`, F16 branch) marks each listener's expression invalid and unslaves it, but leaves the listener's `expressions[i].dependencies` entry on the deleted knob. Its `recheckInvalidExpressionsReferencing` (:970) then runs while the knob is still in the holder's `_knobs` and its Python attribute still exists (`KnobHolder::deleteKnob`, :9571, erases it only afterwards). So the recheck can re-bind listeners to the dying knob. A later `setExpression` on such a listener reaches `removeExpressionDependencies` → `onKnobSlaved` (:4619) and hits `assert(master->getHolder())` (`Engine/Node.cpp:7849`). M372 - User Knob Engine Edits worked around this in b55f57417 by clearing expressions while old knobs are still attached. Fix it in the engine and remove the workarounds. It joins F16 - User Knob Editor and branches off `feature/f16-user-knob-editor`, where the M372 workarounds and the F16 `Knob.cpp` line numbers above live.

## Scope

**In:**
- Drop listener dependencies on a deleted knob; run the recheck once the knob is gone; remove the M372 workarounds.

**Out:**
- Lifetime of knobs read across nodes during a concurrent delete (M507 - Lock-Free Knob Holder Lifetime).

**Assumptions:**
- Expressions resolve knobs through the holder's list or the Python attribute, so a recheck after both are gone cannot find the deleted knob.

## Phase 505.1: Engine fix

- [x] M505.P1.T1 — Drop dependencies on the deleted knob and recheck after it is detached (merged)
  - files: `Engine/Knob.cpp` (`KnobHelper::deleteKnob`, `KnobHolder::deleteKnob`)
  - approach: in the listener loop, call `isHelper->removeExpressionDependencies(i, this)` while this knob still has its holder. Move the recheck out of `KnobHelper::deleteKnob` into `KnobHolder::deleteKnob`, after the `_imp->knobs` erase (collect the names first).
  - verify: `ctest -R "ProjectResetListeners|UserKnobEdit|Expression"` passes at the phase build
  - size: M
- [x] M505.P1.T2 — Regression test: a deleted knob kept alive, then a listener gets a new expression (merged)
  - files: `Tests/ProjectResetListeners_Test.cpp`
  - approach: Grade A's `white` reads B's user knob `k`. Hold a `KnobIPtr` to `k`, then `holder->deleteKnob(k)`. Check behaviour, not asserts (local builds define NDEBUG): A's expression is invalid, `k` has no listeners, A's node has no link to `k`. Then `setExpression` on A's `white` succeeds and evaluates. Old gtest: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: fails before P1.T1 (the listener still points at `k`; debug PR CI asserts); passes after
  - size: M

## Phase 505.2: Remove the M372 workarounds

- [x] M505.P2.T1 — Remove the clear-while-attached workarounds (merged)
  - files: `Engine/PythonUndoCommands.cpp` (`clearUnlinkedExpressions` :830, its call at :879, `clearExpressions()` :1057 and calls at :1199/:1209), `Engine/UserKnobEdit.cpp` (:1507 loop)
  - approach: delete them. Dependents' expressions are re-set as before; check that nothing else relied on the clear.
  - verify: `ctest -R "UserKnobEdit|PythonUndo"` passes, including `UserKnobEdit.AnimatedIntToDoubleKeepsKeysAndDependents` (`Tests/UserKnobEdit_Test.cpp:467`)
  - size: M

**Verification gate:** P1.T2 and the UserKnobEdit suite pass; GUI bench rule (`Engine/Knob*`): `tools/bench/gui/run.sh --xvfb` + `compare.py`, recorded in Decisions; full debug PR CI green.
