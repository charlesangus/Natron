# M201 - Trimap Removal

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

The trimap (`NATRON_ENABLE_TRIMAP`, on at `Global/Macros.h:287`) records in-flight regions with no owner. A thread can wait on its own marks, two renders can wait on each other until aborted, and a finishing render marks regions it never produced as rendered. Instead of adding ownership, find out whether the task graph's `FrameStore` and cache make the trimap redundant once Legacy is gone, and delete it. Moved from M31 - Architectural Cleanup (cancelled 2026-10-25).

Blocked on: M199 - Remove Legacy Scheduler, so removal is evaluated with only the task graph left, then the user's go-ahead on the finding.

Acceptance sketch:
- A finding on what still relies on the trimap (cross-frame duplicate renders, viewer against writer overlap), approved by the user.
- If removal holds: no `NATRON_ENABLE_TRIMAP` code remains, and two overlapping renders of the same image both finish without abort or stale pixels.

Carried from M31.P6.T1 — Stop a thread treating its own marked pixels as rendered elsewhere:

  - files: `Engine/EffectInstancePrivate.cpp` (`waitForImageBeingRenderedElsewhere`, `markImagesAsBeingRendered`), `Engine/Image.cpp` / `Engine/Bitmap` trimap code
  - approach: the trimap bitmap records "being rendered" without an owner, so the wait loop in `waitForImageBeingRenderedElsewhere` counts the calling thread's own marks as another render's. Two threads that each saw the other's marks can wait on each other until the render is aborted. Track ownership of in-flight regions (per render or per thread) so a thread waits only on regions someone else owns.
  - verify: a concurrency test where two renders of overlapping RoIs on the same multi-layer image both finish without abort; whole ctest green in the debug build with assertions live.
  - size: L

Carried from M31.P6.T2 — Mark only this render's own regions rendered when it finishes:

  - files: `Engine/EffectInstanceRenderRoI.cpp` (`ImageBitMapMarker_RAII` destructor), `Engine/EffectInstancePrivate.cpp`
  - approach: on success the RAII destructor marks every layer image rendered over the whole rects-to-render list, which can flag as done a region another thread is still rendering. Mark only the regions this render actually produced, building on the ownership tracking from M31.P6.T1.
  - verify: a test where one render finishes while an overlapping render is still in flight, and a reader of the second region waits instead of reading stale pixels; ctest green.
  - size: L

## Scope

**Out:**
- The per-render ownership fix in the carried briefs. Not planned (user, 2026-10-25).

**Edge cases:**
- Viewer and writer rendering the same non-frame-varying image at once (`ImageKey` omits time): the finding must say what dedupes them without the trimap.

**Assumptions:**
- Unconfirmed: if the finding says the trimap can't go, this milestone is replanned with the user rather than falling back to the ownership fix.

## Decisions

- 2026-10-25 — **Removal over repair, after M199:** the user chose to evaluate deleting the trimap once only the task graph remains, not to fix ownership.
