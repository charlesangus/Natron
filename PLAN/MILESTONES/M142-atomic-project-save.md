# M142 - Atomic Project Save

Make every project save collision-free and loss-free. M16 - Project Format Redesign already moved saving to `Project::writeProjectFileAtomically` (`Engine/Project.cpp` ~:1031): a `QSaveFile` writes a uniquely named temp file beside the target, syncs it and renames it over the target, and `Project::rotateBackups` never removes the target before the commit. The old millisecond-named temp file in the shared temp dir, and its cross-device copy fallback, are gone. What is left are the save paths around that core: errors that report success, autosave scans that can pick up an unfinished temp file, a manual save dropped while an autosave runs, a render save path shared by every instance, backup rotation past nine versions, and directory durability.

Deps: none.

## Scope

**In:**
- A failed save returns failure to every caller (user save, save-before-close, autosave, render save).
- Autosave scans (`findAutoSaveForProject`, the untitled-autosave restore in `Gui/GuiAppInstance.cpp`) accept only real autosave names, never a `QSaveFile` temp file being written.
- A manual save requested while an autosave is running is carried out after it, not dropped.
- The render save (`RENDER_SAVE`, `Engine/AppInstance.cpp` ~:1891) uses a path unique to the instance and render, so two instances can't overwrite each other's file before the render process reads it.
- Backup rotation orders `.~N~` files numerically, so it works up to the 32 versions the setting allows.
- The target directory is fsynced after the final rename.
- Regression tests for concurrent saves, an unwritable directory, temp names in autosave scans, and backup rotation past ten versions.

**Out:**
- Locking against two processes saving the same target at once: last rename wins, as before.
- Changing the bundle format or the backup naming scheme.

**Edge cases:**
- The autosave thread runs the save callback under the Python lock, so a manual save that waits for an autosave must not block the main thread on that lock; queue it to run when the autosave finishes.
- `clearAutoSavesDir` keeps its broad match, so it still removes temp files left by a crash.
- Running as root ignores directory permissions; the unwritable-directory test returns early with a message there (no `GTEST_SKIP` in this gtest).
- Unique render-save files are removed when the render process has loaded them, or at the next autosave-dir cleanup.

**Assumptions:**
- `QSaveFile` uses `O_TMPFILE` where available; on file systems without it (NFS homes) the named temp file exists for the whole write, which is why the autosave scans must reject it.

## Phase 142.1: Save paths around the atomic write

- [x] M142.P1.T1 — Report failed saves and keep in-progress autosaves out of autosave scans
  - files: `Engine/Project.h`, `Engine/Project.cpp`, `Gui/GuiAppInstance.cpp`
  - approach: `saveProject_imp` returns false when the save threw. New `Project::autoSaveProjectNameLength` accepts only names ending in `.ntpz.autosave` or `.ntpz.autosave.<digits>` and rejects `RENDER_SAVE`; `findAutoSaveForProject` and the untitled-autosave restore scan use it.
  - verify: the P1.T3 tests.
  - size: S

- [x] M142.P1.T2 — Manual save during an autosave, unique render saves, numeric backup order, directory fsync
  - files: `Engine/Project.h`, `Engine/Project.cpp`, `Engine/AppInstance.cpp`, `Tests/Serialization/ProjectBundle_Test.cpp`
  - approach: a manual save that finds an autosave running is queued and runs when the autosave finishes (the user sees it complete, not silently dropped). The render save writes a per-instance, per-render unique file beside the autosaves and hands that path to the render process; it is removed once loaded or by `clearAutoSavesDir`. `findBackups` sorts by the parsed version number. After the rename, fsync the target directory.
  - verify: tests for backup rotation with 12 saved versions keeping every version in order, two render saves from one process getting distinct paths, and a manual save requested during a running autosave completing.
  - size: M

- [x] M142.P1.T3 — Regression test for concurrent same-millisecond saves
  - files: `Tests/Serialization/ProjectBundle_Test.cpp`
  - approach: 32 threads released together by a `std::barrier`, 10 rounds. Each thread calls `Project::writeProjectFileAtomically` once on its own target and once on a shared target in the same `QTemporaryDir`. Assert no save throws, each own target reads back its writer's project, the shared target holds one writer's complete project, and the folder lists only the targets (no temp files). Also: an unwritable folder throws, skips rotation and leaves the target byte-identical; the autosave-name check rejects names with the temp suffix.
  - verify: `ctest -R ProjectBundle --repeat until-fail:50`; `ctest -j8 -R "ProjectBundle|KnobShuffleMap" --repeat until-fail:20`.
  - size: S

**Verification gate:** the ProjectBundle tests pass 50 repeated runs and 20 parallel runs alongside `KnobShuffleMap`, and the full ctest passes in both schedulers on a release build.

## Decisions

- 2026-10-24 — **Phase 1 verified (PM):** fast build on 6df447537 is 403/403 on both legs. On that build tree in natron-dev, `ctest -R ProjectBundle --repeat until-fail:50` and `ctest -j8 -R "ProjectBundle|KnobShuffleMap" --repeat until-fail:20` both pass. Design accepted from the implementer: a manual save during an autosave waits for it in place (a local event loop on the main thread that ignores user input, or a condition variable elsewhere) and returns its real result. Queuing it would have needed reworking every caller that needs the result at once (save, save-before-close, `app.saveProject`). Python save calls release the GIL while saving, as `app.render` does. Render saves are `RENDER_SAVE_<pid>_<appID>_<n>.ntpz`, deleted by the render process after loading, and cleaned up once their owning process is gone.
- 2026-10-24 — **Freshness rewrite after M16 (PM):** M16 replaced the save core with `QSaveFile` beside the target and backup rotation that never removes the target, so the original P1.T1 (unique temp file beside the target, drop the copy fallback) is already done. A freshness pass found the remaining gaps. A failed save returned true, so save-before-close closed the project anyway. `QSaveFile` temp names matched the autosave scans. A manual save during an autosave was dropped without a message. `RENDER_SAVE.ntpz` is one path shared by every instance. `findBackups` sorts alphabetically, which breaks rotation at `.~10~`. The directory is not fsynced. The phase is rewritten to cover these. Two are PM calls beyond the original text: the queued manual save and the unique render-save path, both because they lose or misdirect a user's save.
