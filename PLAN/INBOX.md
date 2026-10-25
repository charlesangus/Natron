## Pending

### 2026-10-24T23:30:00Z — info
- refs: none
User request: a Python API for undo steps, as context managers — e.g. `with app.newUndoStep("label"):` groups everything inside into one undo step (nesting folds into the outer step), and `with app.noUndoStep():` runs edits without recording them. Builds on M114 Phase 114.5 (Python edits recorded through the Engine-side UndoRecorderI with UndoStepGuard/UndoSuppressGuard), which exposes no Python API. It also covers edits made from PySide widget handlers in Python panels, which 114.5 leaves unrecorded because they have no C++ entry point. Decide names, where they live (app vs module-level), behaviour with no recorder (background/command line: no-ops), and exception handling (the step still closes; whether a failed block is rolled back).

### 2026-10-25T00:40 — change-request
- refs: none
- New E5 - GPU Compute milestone, user-requested as M156 - GPU Download Throughput In Comps' follow-up, name suggestion "Copy-Compute Contention": in fused comps, in-app device copies run at 7.5–11.7 GB/s down and 5.6–7.9 up, against GpuBench's 16.1 / 10.4 for an isolated host-import copy. GpuBench's overlap-on rows show ~10 / 9.9 whenever a copy runs beside compute. RADV here (RX 7900 XTX, `RADV_PERFTEST=transfer_queue`) gives copies no transfer-only queue in practice (`GpuDevice.cpp:308-334`), so they share the compute queues. Scope: measure whether a dedicated transfer/SDMA queue, or scheduling region copies apart from kernels (no overlap at region edges), recovers the isolated rate; then fix if it does. Data: `.local/m156/p2t2-ka1/`, `.local/m156/p2t1/`, M156's Decisions. Deps: M156.
