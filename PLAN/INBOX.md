## Pending

- 2026-11-13 [bosley-native-nodes] → M345 - VDB Rendering Fixes, M347 - Volume Material Node, M354 (owner on claim): MoonRay is now built from the charlesangus forks pinned by commit (`NATRON_MOONRAY_COMMIT` in tools/ci/local/Dockerfile), with no build-time patches (M325 - MoonRay Fork). Any MoonRay source change these milestones need goes into the fork branch `natron/v2026.29.1`: push the submodule fork, bump its gitlink in charlesangus/openmoonray, then bump the pin. Don't add a sed/patch step to build-moonray.sh. Update their notes on "the override" to match.

### 2026-11-13T00:00:00Z — info
- refs: M345, M354
- From M325 - MoonRay Fork's review (PR #272), deferred because any `tools/ci/local/build-moonray.sh` edit forces a dev-image rebuild: whichever of M345 - VDB Rendering Fixes or M354 - MoonRay Native Shaders next edits that script should also drop the dead `-DBUILD_MATERIALX_SHADERS=OFF` (the fork removed the option), fix the header's stale reason for not fetching materialx_shaders (~:12), reword the SOURCE text so the six unpatched upstream submodules don't read as Natron forks (~:196), and delete or justify the `check_origin` comment (~:58).
