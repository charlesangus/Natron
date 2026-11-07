# Heavy Scene Spike: Results And Mechanism Decision

M327 - Heavy Scene Spike. Decision by the user, 2026-11-07: draw states reach Hydra through a per-engine scene index filter with a GL box overlay, not session-layer draw mode. The 100 ms state-change budget stays, and M329 - Viewer Draw States finds where the frame's time goes on heavily loaded scenes.

Data: `.local/heavy-scene.jsonl` (87 of 92 records, RX 7900-era first run) and `.local/heavy-scene-settle.jsonl` (20 records, AMD Radeon AI PRO R9700), both from the user's GPU host. Synthetic fixtures at 1M prims, plus Lobby (2,786 prims loaded). Island wasn't run. Each scenario ran once, so these are single samples. Prototypes live on `spike/m327-hydra-filter` and aren't merged.

## Mechanisms against the budgets

| ms unless noted | Filter | Session |
|---|---|---|
| Sibling change, loaded 1M city | 1045-1249, visible | 10-15, but 0 pixels on a branch where the filter changed 7 |
| Sibling change, flat 1M | 1484-1718 | 18-316, or about 1900 restoring by value |
| Sibling change, Lobby | 9-11 | 7-8 |
| `/World` pruned then restored: pixels still wrong | 0 (city and Lobby) | 5997 of 5997 (city), 27964 of 27964 (Lobby) |
| Prune / restore, loaded city | 1110 / 1388 | 1887 / 40 |
| Reveal, flat 1M, wide row | 2 fps, unfinished at 120 s | Killed by hand after about 53 min |
| Cancel, flat 1M | 120 | Killed |
| Reveal first frame / cancel, loaded city, row with few children | 1207 cancel in the first run | 29 / 16 in the second session run; 1343 / 1402 in the first |
| Orbit at 10k boxes | 51-60 fps (first run), 84.6 fps on Lobby with 661 boxes | **Hung Natron** on the loaded city; user killed it at 145 s |
| Block load stall, city | 50-51 | 111-117 |
| Swap stall, city | About 80 | 144-154 |
| Peak memory, loaded city | 12.4 GB | 13.6 GB |

Screenshots of Lobby show session's restored image identical to its pruned image: the building never returns. Its prune works. The restore failure may be a spike bug; the hang and the O(children) reveal aren't.

Session's reveal authors a spec per child on the render thread under a lock, because a draw mode opinion can't release part of a subtree (read from `Gui/SceneViewport.cpp` on the spike branch, not measured separately). M329's Children state needs exactly this.

## Engine costs

- Payload loads off the lock: with LoadNone, no unrequested payloads load under either mechanism. Block load holds 0.8-4 s.
- Pattern re-match about 12-15 ms with 60 matches; swap 1.6-2.8 ms with no root resync, on a stage of a few thousand prims. The 1M case is unmeasured.

## Budgets

- Kept: state change at most 100 ms. The filter fails it at 1M loaded (about 1.1 s) and passes on Lobby (about 10 ms). Apply is 1-65 ms, so the cost is in the following frame, probably Hydra or Storm (inferred). No revision proposed.
- Open stall over 100 ms in every open under both mechanisms: 14-23 s first frame at 1M loaded, 2.2 s on Lobby. M328 - Deferred Viewer Stage's work.

## What later milestones change

- **M328 - Deferred Viewer Stage:** the first render (1.3-1.9 s even pruned, 11 s loaded) can't stay on the main thread if the stall gate is to pass. Test load continuity with larger loads.
- **M329 - Viewer Draw States:** build on the filter and the GL overlay. Add: per-change frame cost at full load, adaptive reveal batches, a real first-boxes signal (`first_frame_ms` is a frame, not boxes), markers (360 boxless rows were dropped, so markers are unmeasured), picking on the overlay. Check why the Geometry row on Lobby changed no pixels under either mechanism at 1.1% coverage, and whether a pruned row should hide what the yellow dots show.
- **M331 - Deferred Pattern Matching:** measure swap at 1M, and explain 10 re-match calls when only the last node changed.
- **M335 - Viewer Render Draw States:** verify the filter binds on delegate engines; this is undocumented.
- **M336 - Heavy Scene Acceptance:** add loaded reveal and cancel, and run Island.

## Harness

The latest harness is on the spike branch, with per-scenario limits, a memory guard, picking rows from any stage and a pixel check (`tools/bench/heavy_scene_*`). Its pixel check against a control row is what exposed session's failure. M329 ports it with the filter. A first `settle` run hung the user's machine, cause not established; memory was later ruled out at 46 GB free, and session's orbit hung Natron on a rerun.
