# Handoff — bosley-native-nodes, 2026-11-13 (session restart for the Sysbox fix)

**Before anything:** run `docker start natron-dev`. It failed only because the host's sysbox-fs died.

## Remote gates in flight
Re-attach with `gh run watch <id>`, or read the results with `build.sh release --from-run <id>`.

| Milestone | Run | SHA | Then |
|---|---|---|---|
| M301 - GPU Cache Tier | 43416040786 | 9696c7bdd | green → merge main/F6 into it, PR into F6, review |
| M311 - UDIM Live Textures | 43419804120 | b25e9d1d2 | also records `USDRender/StormUdimTiles.exr` (download it, commit it) |
| M401 - 3D Viewer Breadcrumbs | 43417013167 | 0682e5a6e | green → PR into F4, review |
| M325 - MoonRay Fork | 43430041370 | 52c110e64 | rebuilds the dev image; green → PR (base main or F9 per its row), review |
| M330 - Graph Draw Policy | 43433874523 | 7140d4d7b | 35 GUI scripts; green → PR into F4, review |
| M53 - Projected Texture Painting | 43431969579 | f99992c23 | review fixes for PR #269; green → re-review round, merge into F11 |

## Not yet gated
- **M364 - Delegate Cancel Responsiveness:** 097ad2325, with notes in its milestone file.
- **M280 - Utility Renderer GPU Path:** WIP that doesn't build, on `wip/m280-review-fixes`. See its milestone file.

## Waiting on the user
- F10 UAT (PR #265).
- M399 round 4 UAT (PR #264). Its Xvfb bench rerun is now possible again.
- M268 (#185), M322 (#187) and M398 (#262) UAT.
- The Python `setExpression` question in Open questions.

## Next claims
- F4: M334, M335, M336, M381.
- F6: M303.
- F7: M281–M284, M288.
- F11: M312–M315.
- F15: M361 and M337, after F4 lands.
- F9: M344, after M325.
