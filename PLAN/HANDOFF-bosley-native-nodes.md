# Handoff — bosley-native-nodes, 2026-11-14 (host restart)

**Before anything:** `docker start natron-dev` (check `docker ps`); reinstall `pip install --user --break-system-packages clang-format==21.1.8` if the pre-commit hook says it's missing. No subagents were running at shutdown and no worktree has uncommitted work. Remote runs keep going on GitHub; read results with `tools/ci/remote/build.sh release --from-run <id>` or the job logs. Codex quota was exhausted (5h) at ~15:00 — check `codex-review.sh check`; fall back to a Claude `cat-reviewer`.

## Main merge window (land together, then merge main into every open Feature/lane once)
| PR | What | State | PR CI run |
|---|---|---|---|
| #284 `tests/cycles-restart-flake` | Cycles restart race fix (offline + progressive), eviction false-positive fix, GraphScalingConnectTest 900 s timeout | release gate green (a3a0090cb); Codex round closed | 43629986125 |
| #288 M424 | Python setExpression refreshes viewer | gate green apart from the Cycles race; review closed; teardown-assert fix 37d8f90e0 | 43639474432 |
| #289 M425 | ORT telemetry off (`ORT_DISABLE_TELEMETRY` in every main) | gate green apart from Cycles race; review closed (536c8a253) | 43632038095 |
Merge order: #284 first, then #288, #289. Then merge main into F4, F6, F7, F9, F11, F15 branches and their open members.

## Lanes
| Milestone | State | Next |
|---|---|---|
| M334 - Knob Prim Tools (PR #283 → F4) | GUI all green; review round closed | after #284 reaches F4: merge F4, re-gate, merge |
| M335 - Viewer Render Draw States (PR #281 → F4) | ctest green; m254 pause checks fail due to the lane's own RestartWatch | merge F4 (with #284) taking main's `RestartWatch`, drop the lane copy, final gate, merge |
| M381 - Deferred Matching Coverage (PR #278 → F4) | release + PR CI green apart from Cycles race | after #284 reaches F4: merge into F4 |
| M336 - Heavy Scene Acceptance (F4) | bench scenario written (4857cee1a) | GPU-host run of `heavy_scene_gpu_host.sh acceptance`; then UAT AppImage |
| M303 - Interactive Edit Latency (PR #287 → F6) | release gate green; PR CI on a37af740c failed only GraphScalingConnectTest timeout (fixed by #284's timeout) | after #284, rerun PR CI → merge into F6 → F6 ship |
| M281 - Utility Materials And Lights (→ F7) | gate running on b5ac481c3: run 43637262978 | green → PR into F7, review |
| M283 - Utility Sampling And Motion Blur (PR #286 → F7) | gate running on ba098b912: run 43634814939; review round closed | green → merge into F7 |
| M284 - Utility Renderer Subdivision (F7) | P1.T3 golden waits on M281; last build only had cage-pin fixes (a3a7eea74) | rebuild after M281 merges into F7, record golden, PR |
| F9 - Delegate Projection (PR #282, shipping) | gate running on d9ada1194: run 43627808852; review fixes in; one finding (reload stall) fixed via park-first barrier | green → set `Needs: uat`, package UAT AppImage + steps .md; follow-up: Cycles-pick GUI cost ~95–107 ms, abandoned Cycles sync contention on switch to Storm (decision for user if first-Storm-frame check still fails) |
| M272 - Script Writes To Selected Roto Shapes (PR #285 → main) | gate running on fecb2c729: run 43633168691 (render-end deadlock fix) | green → merge to main; then M421 merges main and re-gates |
| M421 - Stroke Keyframe Indicators (PR #279) | waits for M272 | merge main after M272, re-gate (XFAIL steps must pass) |
| M136 - Edge Policy Investigation | gate running on 15e2f9e41 with `--record-goldens`: run 43627622335 | open: ShuffleCopy Stretch==Black + GPU fallback, IDefocus GPU≠CPU, IDistort/STMap UV + ZDefocus Depth rows; triage in milestone Decisions |
| M221 - Neural Optical Flow (F8) | gate green apart from MoonRay flake | waits on user: model hosting release + licence (Open questions) |
| M376 - Geometry Display (F15) | gate green | PR into F15 after F4 lands |
| M53 - Projected Texture Painting | **done**, merged into F11 (a67a83bd0) | — |

## Next claims
- F11: M312–M315 (all stubs; deps satisfied now M53 is in F11) — elaborate via consultant, then implement. M427–M430 wait for F15 to land.
- F7: M282, M288 after M281 + M283 merge.

## Waiting on the user
- M221 model hosting/licence; M272 Python selection API defaults; Storm primId on projected cards follow-up (Open questions).
- UATs: F9 (once gated), F4, F6, F7, F11 when shipped.
