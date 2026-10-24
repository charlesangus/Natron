# M167 - Per-Frame Node Metadata

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Make a node's metadata (colour layout and components, bit depth, PAR, fps, format, field order) a function of time and view rather than one value per refresh. Each node computes it on request at (time, view) and caches it keyed by node hash, time and view, as M77 - Native Metadata Nodes does for key/value metadata. Routing nodes (Switch, integer Dissolve, TimeOffset, FrameHold) then report their active input's metadata at each frame. Native nodes derive it at a time; OFX nodes keep one time-invariant answer per hash, since getClipPreferences has no time. Consumers outside the render (Viewer, Write, node graph, Python, project format) ask at an explicit time, and Write fixes one header per file. Premult is not a metadata field: M43 - Drop Premult Concept removes it.

Filed 2026-10-24 by M110 - Remaining Native GPU Ports, on the user's ruling that node metadata is not the same at every frame (M110's `## Decisions`, 2026-10-24); sized XL by its scoping consultant. M110.P3.T4 already fixes the Switch's per-frame layers through the components path, ahead of this. It absorbs M144 - Render-Thread Metadata Refresh (cancelled 2026-10-24): hash-keyed, on-request metadata was one of M144's two candidate designs and removes the main-thread-only refresh, so M144's acceptance is carried below.

Deps: M100 - Content Node Hashing.

Blocked on: a decision on what a Write does when its input's metadata varies across its range, and the design pass M144 - Render-Thread Metadata Refresh was waiting on (now settled in favour of hash-keyed metadata; its spike folds into this milestone's first phase).

Acceptance sketch:
- A Switch keyed rgb+spec at f1 and rgba at f2 reports RGB metadata and layers at f1 and RGBA at f2, and so does every node downstream, including AddLayers' getPreferredMetadata.
- The Viewer's format, PAR and channel menu follow the current frame; a Write over a varying range applies the agreed policy.
- An OFX node's clip preferences are computed once per hash, and OFX tests are unchanged.
- Metadata is queried off the main thread with no deadlock or stale value.
- From M144 - Render-Thread Metadata Refresh: a file render, a tracker render and a colour-sampler render whose upstream changes mid-render never cache or output a frame computed from stale input-related data, none of them deadlocks or fails the job because of the edit, and a regression test edits a node during a non-abortable render and checks the cached frame's hash against its inputs.
- `NodeMetadata` gains no premult field; OFX premult stays M43 - Drop Premult Concept's constant answer, and `PremultAbsence_Test` stays green.
- Scheduler-equivalence, cache-equivalence and TimeVaryingLayers suites are green.
