## Pending

### 2026-11-16T03:30:00-05:00 — change-request
- refs: none
- New milestone requested (user, 2026-11-16, node graph UAT of M397 - Label Resize Keeps Node Top-Centre / M374 - Remove Node Magnify On Ctrl-Scroll): node error and warning text is drawn over the node in a very ugly way and should be redesigned. Suggested name "Node Error And Warning Display", E10. Not part of the M397/M374 fixes.

### 2026-11-16T03:31:00-05:00 — change-request
- refs: none
- Follow-up milestone requested (user, 2026-11-16, F8 - Motion Vectors UAT): put follow results in a separate animation layer, so the user keeps the benefit of the follow and can still keyframe the shape on top; the keyframe-reduction pass could then run on the follow layer. Suggested name "Follow Animation Layer", E10. The rest of that UAT round is being fixed on the F8 branch.

### 2026-11-16T03:32:00-05:00 — change-request
- refs: none
- Fold the project-wide decision `DECISIONS/2026-11-16-deep-opacity-output.md` (deep opacity, alpha-only deep output) into the scope of M286 - Utility Renderer Deep Output, M257 - Cycles Deep Output and M258 - MoonRay Deep Output. Also fold `DECISIONS/2026-11-16-moonray-deep-over-arras.md` (MoonRay deep over banded Arras DeepFrame messages) into M258, and the M255 - Delegate Output Spikes findings (Cycles kernel 2 KB/px slab, merge threshold, flatten mismatch, empty flattened AOV, no ids) into M257.
