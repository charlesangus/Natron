# M380 - Blocking Scheduler Aborts

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

`GenericSchedulerThreadPrivate::waitForAbortToComplete_internal` (`Engine/GenericSchedulerThread.cpp`) tests `(state == eThreadStateAborted) || GenericSchedulerThread::eThreadStateIdle`. The second operand is a non-zero enum constant, so the condition is always true, and every "blocking" abort returns immediately. The bug comes from upstream. Callers that rely on the wait can free state the worker is still using: the render engine, the tracker (`abortTracking_blocking`), previews, the backup thread, and `Node::abortAnyProcessing_blocking`. M219 - Curves Follow Vectors hit this as a segfault and worked around it with its own mutex.

Fix the condition. Then each caller really does block the main thread, so audit every caller for deadlocks. Also check that each worker notices an abort quickly, to keep the UI-responsiveness rule (renders cancel fast). Once the wait blocks, M219's workaround mutex may no longer be needed. Remove it if so.

Blocked on: the per-caller deadlock and abort-latency audit. Whether some callers need a non-blocking path, a timeout, or a worker-side change can't be known until that audit runs.

Acceptance sketch:
- A blocking abort waits until the worker is aborted or idle. A unit test proves it by having a worker that holds state across the abort.
- No caller deadlocks. An abort latency check per worker type shows the main-thread wait stays within the responsiveness budget.
- M219's workaround mutex is removed, or the reason it has to stay is recorded.
