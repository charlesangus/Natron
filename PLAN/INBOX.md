## Pending

### 2026-10-16T09:00:00Z — change-request
- refs: none
- New milestone requested (user accepted the miss in M90 - Render Path Serial Overhead): **Blur In-Graph Overhead** (E6). In-graph native Blur at UHD on 16 threads is 3.1× its bare kernel at FIR σ3 (4.70 vs 1.51 ms/Mpx) and 2.0× for IIR (6.83 vs 3.37). Profile on the 16-thread host (symbolised build) and cut the fixed per-task overhead (fill, `writeWindow` finish, planar buffer allocation) and the padded-region cost at large σ. Data: `M90-render-path-serial-overhead.md` Decisions 2026-10-16. Deps: M90.


### 2026-10-16T12:30:00Z — change-request
- refs: none
- New milestone requested (user): **Native Metadata Utilities** (E3, after M77 - Native Metadata Nodes). Native versions of the OFX example metadata nodes that M77 left without a native equivalent — `org.openfx.examples.metadataCompare`, `metadataContribute` and `metadataTimeCode` — then retire the OFX ones from the bundle and the asset fetch. Also replace the three OFX example plug-ins the test suite uses as metadata-bridge fixtures (Contribute, Print, TimeCode in `build/assets/TestPlugins/`) with an in-repo test-only OFX plug-in, so the bridge keeps coverage once they're gone. Deps: M77.

### 2026-10-16T13:40:00Z — change-request
- refs: none
- New milestone requested (user): **Complete Local Build Environment** (E2). Make the local dev workflow self-sufficient and current, so a PM or implementer can verify GPU code, run the debug ctest PR CI runs, and package without relying on remote builds. Findings from 2026-10-16:
  - The natron-dev image has no `slangc`. `cmake/FetchSlang.cmake` downloads Slang into `<checkout>/.natron-deps` at configure time, which needs network the container may lack. On the bosley host it was seeded by hand from `/opt/slang`, and each worktree needs a symlink.
  - With the host in remote build mode, no local tree postdated M85, so `build/syntax-check.py` silently skipped every GPU translation unit. Implementers then wrote stub kernel headers by hand.
  - `build/syntax-check.py` has no `-I Tests`/`-I libs/xxhash` for test TUs (every agent adds them by hand). It takes a checkout root and base ref, not file paths, which agents keep getting wrong.
  - Remote builds (`tools/ci/remote/build.sh`) offer only `fast`/`release` (both NDEBUG), while PR CI runs debug with live asserts and a parallel ctest that hides assert text (`natron-pr-ci-debug-asserts` memory).
  - `build.sh` resets submodules, and worktree lanes need their own `fetch-assets` and `.natron-deps`.

  Scope ideas:
  - Add Slang (and anything else the build fetches at configure time) as an image layer.
  - Keep a maintained local GPU-era tree, or have syntax-check configure one on demand.
  - Fix syntax-check's include paths and CLI.
  - Add a `debug` preset to the remote build and `-V` output for failing cases.
  - A worktree bootstrap script (submodules, fetch-assets, `.natron-deps`, appimagetool wrapper).
  - Document it in `tools/ci/local/README.md`.

  Deps: none.

### 2026-10-16T14:10:00Z — change-request
- refs: none
- Bug, found by PM in PR CI (PR #64, legacy ctest, run 38894698099): `Project::saveProjectInternal` (`Engine/Project.cpp` ~:777) writes the project to a temp file named from `QDateTime::toMSecsSinceEpoch()` in the temp dir. Two processes saving in the same millisecond collide: parallel ctest did this, and so would two Natron instances. The first save fails with "device not open" and `Failed to save`. It made `ProjectSaveLoadPreservesKnobShuffleMapRows` and `...UserKnobShuffleMapRows` fail intermittently. Fix with `QTemporaryFile` or `QSaveFile` beside the target. Small; fits E2 or E7.


### 2026-10-16T16:30:00Z — change-request
- refs: none
- New milestone requested (user accepted a known-plugin-bug list in M100 - Content Node Hashing's cache-equivalence sweep): **Bundled Plugin Bug Fixes** (E3). Fix the three bundled-plugin bugs the sweep found, in our openfx-misc and openfx-arena forks, then bump the bundled assets and empty the sweep's known-plugin-bug list:
  - CImg EdgeExtend `mergeOver` assumes 4 channels and reads out of bounds when processA is off;
  - the CImg expression parser reads an uninitialised variable in CImg.h (patch at `build/ceq/cimg-undefined-variable.patch` on the bosley host);
  - fxarena Sketch uses unseeded noise, so its renders are non-deterministic; give it a fixed seed.
  
  Deps: M100.

### 2026-10-19T08:00:00Z — change-request
- refs: none
- Follow-ups from M100 - Content Node Hashing (merged d521f111a), found by PM:
  1. **A Write edited mid-render can cache a stale frame.** Off-main viewer renders that find stale input-related data abort and re-render (Engine/ParallelRenderArgs.cpp, ViewerInstance.cpp). File renders and non-abortable renders (tracker, colour sampler) are left as they were, because aborting fails the job, and a blocking main-thread wait risks deadlock. Needs a design for refreshing input-related data safely from a render thread, or keying Natron's own metadata by node hash.
  2. **A known-bug plug-in child hangs under the taskgraph scheduler.** `CacheEquivalenceKnownPluginBugTest.KnownPluginBugsStillCrash` passes only because its 60 s child watchdog fires (62.9 s in taskgraph vs 3.3 s legacy, release run on 5d5bf9e50). The segfault-handler exit deadlock is already fixed, so this is a different hang: either CImg EdgeExtend or the CImg expression plug-in hangs rather than crashing when rendered on the pool. Get a stack (eu-stack) of the child to tell plug-in UB from an engine deadlock reached after a plug-in fault. It may fold into the requested Bundled Plugin Bug Fixes milestone.
  Suggested: E14 (Architecture) or E7.
