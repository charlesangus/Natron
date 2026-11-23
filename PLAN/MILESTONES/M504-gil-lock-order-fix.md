# M504 - GIL Lock Order Fix

Every wait in `Engine/Nodes/Scene/SceneWriteBarrier.cpp` (F4 branch) that has to wait creates its `PythonUnlockedWhileWaiting` after taking `s.mutex`: `waitAllParked` (:259/:263) and `ReadScope::ReadScope` (:341/:343). The guard is destroyed while the mutex is still held, so the GIL is re-taken under the mutex. A thread that holds the GIL and wants `s.mutex` then deadlocks against it. M336 - Heavy Scene Acceptance (d722c0262) fixed the same pattern in `Gui/SceneNodeStageSource.cpp` (`joinReaped` :246, `stopAll` :256) by declaring the guard before the lock. Fix the barrier the same way and replace the three copies of the class with one.

## Scope

**In:**
- Lock order in `waitAllParked` and `ReadScope::ReadScope`; one shared `PythonUnlockedWhileWaiting`; a deadlock regression test.

**Out:**
- `PythonGILUnlocker` (`Engine/AppManager.h:1024`): it releases unconditionally and asserts, which is different behaviour, so it stays.

**Edge cases:**
- `runExclusive` (:444–449) re-takes the GIL while holding `writerMutex`. Every taker of `writerMutex` releases the GIL first, so it stays as it is, with a comment saying why.
- `waitForAsyncWrites` (:570), and the joins/resets in SceneViewport (:7426, :7714, :7994) and SceneNodeStageSource (:219), hold no lock while the guard is alive, so they are unchanged.

**Assumptions:**
- M504 stays standalone and waits for F4 - Heavy Scene Viewer to land on `main`: the SceneViewport and SceneNodeStageSource copies of `PythonUnlockedWhileWaiting`, and the line numbers above, exist only on `feature/f4-heavy-scene-viewer`.

## Phase 504.1: Fix

- [x] M504.P1.T1 — Re-take the GIL only after the barrier mutex is released
  - files: `Engine/Nodes/Scene/SceneWriteBarrier.cpp`
  - approach: in `waitAllParked` and `ReadScope::ReadScope`, declare `std::optional<PythonUnlockedWhileWaiting> unlocked;` before the `unique_lock`. On the wait path, `lock.unlock(); unlocked.emplace(); lock.lock();` then `wait`, so the lock is destroyed before the guard. The no-wait path still never touches the GIL. Comment the `runExclusive` invariant.
  - verify: by reading, no guard outlives a held `s.mutex`; `ctest -R "ProgressiveRender|SceneLayerMemo"` passes at the phase build
  - size: M
- [x] M504.P1.T2 — A deadlock regression test for the reader wait
  - files: `Tests/Scene/ProgressiveRender_Test.cpp` (next to the ReadScope test at :1670)
  - approach: writer W sits inside `runExclusive`. Reader R, holding the GIL (`PyGILState_Ensure`), opens a `ReadScope` and waits. Thread G takes the GIL. W returns, and ~100 ms later G opens a `ReadScope`. The main thread waits up to 10 s for all three to finish; on timeout it calls `ADD_FAILURE` then `std::_Exit(1)` (threads in a deadlock cannot be joined). Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: hangs or fails with P1.T1 reverted in a scratch build; passes with it
  - size: M

## Phase 504.2: One shared guard

- [x] M504.P2.T1 — Move `PythonUnlockedWhileWaiting` into `Engine/AppManager.h` beside `PythonGILLocker`
  - files: `Engine/AppManager.h`, `Engine/AppManager.cpp`, `Engine/Nodes/Scene/SceneWriteBarrier.cpp` (:107), `Gui/SceneViewport.cpp` (:228), `Gui/SceneNodeStageSource.cpp` (:49)
  - approach: one class that is non-copyable (copy deleted, as in the Gui copies), with its comment generalised; delete the three anonymous-namespace copies.
  - verify: `git grep -c "class PythonUnlockedWhileWaiting"` returns 1; the phase build compiles
  - size: M

**Verification gate:** P1.T2 passes 20/20 (`--repeat-test ProgressiveRender:20`); full debug PR CI green.

## Decisions
- 2026-11-22 — Phase 504.1 green (6a31005b0): full fast ctest plus ProgressiveRenderTest ×10, both schedulers. registerParticipant/unregisterParticipant also release the GIL before writerMutex (PM default for the open question), so every writerMutex taker releases the GIL first. Phase 504.2 and the PR wait for F4 to land (main frozen until then, user 2026-11-22).
- 2026-11-22 — P2.T1 in 280e119aa (definitions live in SceneWriteBarrier.cpp, which already builds without Py_LIMITED_API). PR #341 to main; gate with ProgressiveRenderTest ×20 queued.
- 2026-11-23 — Merged to main as 9f8778831 (#341); review round closed.
