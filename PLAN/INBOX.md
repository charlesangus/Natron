## Pending

### 2026-10-24T19:30 — info
- refs: none
- `Tests/gui/m55_node_graph.py` section `splice-vertical` fails 6 checks on main (seen on `M80-final.AppImage`, main plus M80, and on M134's branch): the hover highlight isn't drawn, the splice connections aren't made, and redo doesn't re-apply them. Found while gating M134 - Output Picker Pixel Aspect, which doesn't touch the node graph. Needs an owner: likely a regression from a node-graph change, or a script assumption that no longer holds.

### 2026-10-24T19:05 — change-request
- refs: none
- New E2 - Build, CI And Release milestone, user-requested from the gpu PM's build/test efficiency review: (b) give `.github/workflows/remote-build.yml` and `tools/ci/remote/build.sh` a single-scheduler `--tests` option for intermediate fix builds, keeping both scheduler legs (legacy and taskgraph, `remote-build.yml:102-122`) for final milestone gates. Today every `--tests` run does the whole ctest suite and the smoke test twice. (a) Have `tools/ci/local/build.sh` derive `JOBS` from the host (`nproc`, and MemAvailable per compile job) instead of the hardcoded 3 (`build.sh:275-282`, sized for a 4-core 15 GB host), and **always leave at least one core free** (user). Keep the `NATRON_BUILD_JOBS` override. Deps: `-`.
- Addendum (2026-10-24, gpu PM): **`tools/ci/local/build.sh` drops its arguments on main** since M154 - Fresh Buffer Page Zeroing (#79). The parse loop (~:99) shifts `$@` empty before the host-side re-exec (~:263, `... build.sh "$@"`), so every host invocation runs as `debug` without `--tests`. The fix is to save `ORIG_ARGS=("$@")` before the loop and re-exec with `"${ORIG_ARGS[@]}"` (the diff is in the gpu PM's scratchpad; it is applied uncommitted in `.worktrees/m110` only for P4.T3's RADV build). It belongs in this new milestone, or in an earlier hotfix.

