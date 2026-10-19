# M142 - Atomic Project Save

Make `Project::saveProjectInternal` (`Engine/Project.cpp:746`) safe against concurrent saves. It writes the project to a temp file named only from `QDateTime::toMSecsSinceEpoch()` in the shared temp directory (`:797-800`), then renames it onto the target (`:861`). Two saves that start in the same millisecond share one temp path. The first save's stream ends up on a file the second has already opened, truncated or renamed away. The first save then fails with "device not open", and the caller sees `Failed to save`.

Parallel ctest hit this: `ProjectSaveLoadPreservesKnobShuffleMapRows` and `ProjectSaveLoadPreservesUserKnobShuffleMapRows` (`Tests/KnobShuffleMap_Test.cpp:317,387`) failed intermittently. Two Natron instances, or an autosave (it runs on a `QtConcurrent` thread, `Project::onAutoSaveTimerTriggered`) racing a user save in one instance, can collide the same way.

The fix: the temp file gets a unique name created with exclusive-create semantics (`QTemporaryFile`), in the target's own directory. That makes it collision-free, and also guarantees the final rename is a same-filesystem atomic rename, so the cross-partition copy fallback goes away.

Deps: none.

## Scope

**In:**
- A unique temp file beside the target for every save path through `saveProjectInternal`: user save, autosave and render save (`RENDER_SAVE`).
- Removing the `QFile::copy`/`fileCopy` retry fallback that only existed for the cross-partition temp-dir case, and the temp file's cleanup on every failure path.
- A regression test that fails on the old millisecond-named temp file.

**Out:**
- Cleaning up stale temp files left by a crashed process: none (the old scheme left them in the temp dir too).
- Locking against two instances saving the *same target* concurrently: last rename wins, as before. Project lock files are untouched.
- Changing the backup rotation (`.~N~` files) semantics or the autosave naming scheme.

**Edge cases:**
- The temp name must not contain `.ntp.autosave` or `RENDER_SAVE`. `findAutoSaveForProject` (`Engine/Project.cpp:982`) and `clearAutoSavesDir` match on those substrings, and a transient temp file in the same directory must not be mistaken for an autosave. Use a fixed template such as `natron-save-XXXXXX.tmp`, not one derived from the target name.
- Backup rotation (`findBackups`, `:696`) matches `\.~\d+~$` against the target name only, so the temp file is never rotated.
- `runOnProjectSaveCallback` may redirect `filePath` to another directory (`:793`). The temp file goes beside the final `filePath`, so it is created after the callback returns.
- Target directory not writable: `QTemporaryFile::open()` fails and the save throws `Failed to open file <path>`, as the old code did for the temp dir.
- Serialization throws midway: the temp file is removed (`QTemporaryFile` auto-remove) and no partial file replaces the target.
- A file already at the target is replaced by the rename. Backups were rotated first, so the order stays: rotate, then rename.

**Assumptions:**
- Unconfirmed: a Natron project directory is writable whenever the save would succeed, so no test relies on a read-only target dir falling back to the temp dir.
- Unconfirmed: `Engine/CMakeLists.txt` lists sources explicitly, so a new helper file needs an entry. Check before adding.

## Phase 142.1: Collision-free save

- [ ] M142.P1.T1 — Save through a unique temp file beside the target
  - files: `Engine/ProjectSaveFile.h` (new), `Engine/ProjectSaveFile.cpp` (new), `Engine/Project.cpp`, `Engine/CMakeLists.txt` (only if sources are listed)
  - approach:
    - Add a small helper that, given the final path, creates a `QTemporaryFile` in the same directory (template `natron-save-XXXXXX.tmp`, `setAutoRemove(true)`), opens it (exclusive create), closes the handle and returns the unique path. The caller keeps the object alive until the rename succeeds, then calls `setAutoRemove(false)` or renames through the helper's `commit()`.
    - In `saveProjectInternal`, replace the `StandardPaths::eStandardLocationTemp` + `toMSecsSinceEpoch()` name (`:797-800`) with the helper, created after `runOnProjectSaveCallback` so it sits beside the real target. Keep `FStreamsSupport::open` on the unique path, since boost's `xml_oarchive` needs a `std::ostream`.
    - Replace the rename/`QFile::copy`/`fileCopy` retry block (`:861-876`) with a single `QFile::rename`, removing the target first if needed (Qt's rename fails on an existing target). Throw `Failed to save to <path>` when that fails. Delete `fileCopy` (`:~660-694`) if nothing else uses it.
    - Keep backup rotation exactly where it is, before the rename.
    - Check the autosave path (`autoSave` true, `Project::autoSave` at `:908`, and `saveProject_imp` at `:640-654`) goes through this same code, so no second temp-name site exists. `grep toMSecsSinceEpoch Engine/` for others.
  - verify: the new test (T2) passes; `ctest -R "Project|KnobShuffleMap|AddLayers|DataKindProjectLoad"` passes; a save leaves no `natron-save-*` file in the target dir.
  - size: M

- [ ] M142.P1.T2 — Regression test for concurrent same-millisecond saves
  - files: `Tests/ProjectSave_Test.cpp` (new; register in `Tests/CMakeLists.txt` if test sources are listed, not globbed)
  - approach:
    - Unit-test the T1 helper directly: N (>=32) threads released together by a barrier each create a temp file beside one target path in a `QTemporaryDir`. Assert every returned path is distinct, every file exists, and each can be opened and written independently. This fails on the old scheme, whose name is a function of the clock only.
    - Add a project-level test with a `BaseTest` fixture: save one project to two different files from two threads at once, repeated 20 times. Assert both calls return true and both files reload with the same node count. If `Project::saveProject` is not thread-safe for one `Project` object, drop this half and say so in the test file; the helper test carries the regression.
    - Assert no `natron-save-*.tmp` remains in the directory after the saves, and after a save that throws (target dir removed mid-test, or an unwritable directory).
    - Use `QTemporaryDir` for every directory.
  - verify: `ctest -R ProjectSave` passes 50 repeated runs (`--repeat until-fail:50`) and in parallel with `KnobShuffleMap` (`ctest -j8 -R "ProjectSave|KnobShuffleMap" --repeat until-fail:20`). To prove the test bites, it fails against the old naming (temporarily hard-code the clock-only name in the helper).
  - size: M

**Verification gate:** `ProjectSave_Test` passes, and `ProjectSaveLoadPreservesKnobShuffleMapRows` and `ProjectSaveLoadPreservesUserKnobShuffleMapRows` pass 20 consecutive parallel `ctest -j` runs; then the full ctest passes in both schedulers (legacy and taskgraph) on a release build.
