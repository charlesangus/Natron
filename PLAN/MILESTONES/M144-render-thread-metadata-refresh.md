# M144 - ~~Render-Thread Metadata Refresh~~

> Cancelled 2026-10-24 (user decision): absorbed by M167 - Per-Frame Node Metadata, whose hash-keyed, on-request metadata is one of this milestone's two candidate designs and removes the main-thread-only refresh. Its acceptance is carried there. Kept for history.

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Stop a Write that is edited mid-render from caching a stale frame. M100 - Content Node Hashing made off-main viewer renders that find stale input-related data abort and re-render (`Engine/ParallelRenderArgs.cpp`, `ViewerInstance.cpp`). File renders and non-abortable renders (tracker, colour sampler) were left as they were: aborting fails the job, and a blocking main-thread wait risks deadlock. This milestone gives those render paths a safe way to get current input-related data, either by refreshing it from a render thread or by keying Natron's own metadata on the node hash.

Deps: M100 - Content Node Hashing.

Blocked on: a design choice between refreshing input-related data safely from a render thread and keying Natron's metadata by node hash. The two touch different code, so the tasks wait on it. A short spike or a `/cat-discuss` design pass settles it.

Acceptance sketch:
- A file render, a tracker render and a colour-sampler render whose upstream changes mid-render never cache or output a frame computed from stale input-related data.
- None of them deadlocks or fails the job because of the edit.
- A regression test edits a node during a non-abortable render and checks the cached frame's hash against its inputs.
