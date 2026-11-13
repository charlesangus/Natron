## Pending

- 2026-11-13 [bosley-native-nodes] → M345 - VDB Rendering Fixes, M347 - Volume Material Node, M354 (owner on claim): MoonRay is now built from the charlesangus forks pinned by commit (`NATRON_MOONRAY_COMMIT` in tools/ci/local/Dockerfile), with no build-time patches (M325 - MoonRay Fork). Any MoonRay source change these milestones need goes into the fork branch `natron/v2026.29.1`: push the submodule fork, bump its gitlink in charlesangus/openmoonray, then bump the pin. Don't add a sed/patch step to build-moonray.sh. Update their notes on "the override" to match.
