## Pending

### 2026-10-16T12:30:00Z — change-request
- refs: none
- New milestone requested (user): **Native Metadata Utilities** (E3, after M77 - Native Metadata Nodes). Native versions of the OFX example metadata nodes that M77 left without a native equivalent — `org.openfx.examples.metadataCompare`, `metadataContribute` and `metadataTimeCode` — then retire the OFX ones from the bundle and the asset fetch. Also replace the three OFX example plug-ins the test suite uses as metadata-bridge fixtures (Contribute, Print, TimeCode in `build/assets/TestPlugins/`) with an in-repo test-only OFX plug-in, so the bridge keeps coverage once they're gone. Deps: M77.

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
