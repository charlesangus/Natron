## Pending

### 2026-10-24T19:30 — info
- refs: none
- `Tests/gui/m55_node_graph.py` section `splice-vertical` fails 6 checks on main (seen on `M80-final.AppImage`, main plus M80, and on M134's branch): the hover highlight isn't drawn, the splice connections aren't made, and redo doesn't re-apply them. Found while gating M134 - Output Picker Pixel Aspect, which doesn't touch the node graph. Needs an owner: likely a regression from a node-graph change, or a script assumption that no longer holds.

### 2026-10-24T19:05 — change-request
- refs: none
- New E2 - Build, CI And Release milestone, user-requested from the gpu PM's build/test efficiency review: (b) give `.github/workflows/remote-build.yml` and `tools/ci/remote/build.sh` a single-scheduler `--tests` option for intermediate fix builds, keeping both scheduler legs (legacy and taskgraph, `remote-build.yml:102-122`) for final milestone gates. Today every `--tests` run does the whole ctest suite and the smoke test twice. (a) Have `tools/ci/local/build.sh` derive `JOBS` from the host (`nproc`, and MemAvailable per compile job) instead of the hardcoded 3 (`build.sh:275-282`, sized for a 4-core 15 GB host), and **always leave at least one core free** (user). Keep the `NATRON_BUILD_JOBS` override. Deps: `-`.

