## Pending

### 2026-11-13T00:00:00Z — info
- refs: M345, M354
- From M325 - MoonRay Fork's review (PR #272), deferred because any `tools/ci/local/build-moonray.sh` edit forces a dev-image rebuild: whichever of M345 - VDB Rendering Fixes or M354 - MoonRay Native Shaders next edits that script should also drop the dead `-DBUILD_MATERIALX_SHADERS=OFF` (the fork removed the option), fix the header's stale reason for not fetching materialx_shaders (~:12), reword the SOURCE text so the six unpatched upstream submodules don't read as Natron forks (~:196), and delete or justify the `check_origin` comment (~:58).
