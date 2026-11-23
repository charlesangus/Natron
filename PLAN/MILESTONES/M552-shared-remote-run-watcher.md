# M552 - Shared Remote Run Watcher

Each `tools/ci/remote/build.sh` watches its own run with `gh run watch --interval 30` (hard-coded `WATCH_INTERVAL_SECS`), about 4 API calls per watcher every 30 s. Ten gates in flight use ~4,500 of the 5,000 hourly core calls (measured 2026-11-23: 920 calls in 12.5 min), so dispatches stall on quota. Replace the per-gate polling with one watcher that polls every run at once, so API use stays flat however many gates run.

Stop-gap in place (PM, 2026-11-23): the PM's gate queue puts a `gh` shim first in PATH that turns `gh run watch` into a no-op, leaving build.sh's own one-call status loop.

## Scope

In scope: `tools/ci/remote/build.sh` waiting and run-finding, a new shared watcher script, `tools/ci/remote/README.md`. Out of scope: PR-check watching, the workflow itself.

## Phase 552.1: Interval and shared watcher

- [x] M552.P1.T1 — Configurable polling, no `gh run watch`
  - files: `tools/ci/remote/build.sh`
  - approach: `WATCH_INTERVAL_SECS` and `FIND_INTERVAL_SECS` read env overrides (`NATRON_REMOTE_WATCH_INTERVAL_SECS`, default 60; `NATRON_REMOTE_FIND_INTERVAL_SECS`, default 10). Drop `gh run watch`; the existing one-call status loop waits for completion and prints a one-line status change when the run's job states change (from the same call, `--json status,conclusion,jobs` only if it stays one call).
  - verify: a dispatch with default settings completes and reports as before; the log shows status lines.
  - size: S
- [x] M552.P1.T2 — One watcher for all runs
  - files: `tools/ci/remote/watch-runs.sh` (new), `tools/ci/remote/build.sh`, `tools/ci/remote/README.md`
  - approach: `watch-runs.sh <state-dir>` polls `gh run list --workflow remote-build.yml --limit 100 --json databaseId,status,conclusion,displayTitle,headSha` once per interval (default 60 s) and atomically writes `<state-dir>/<run-id>` (`status conclusion`) and `<state-dir>/titles.tsv`; it holds a flock so only one runs per state dir, and exits after an idle period with no in-progress runs. With `NATRON_REMOTE_RUN_STATE_DIR` set, build.sh starts the watcher if none holds the lock, finds its run by token in `titles.tsv` and waits on its state file instead of calling gh; with it unset, behaviour is T1's. A stale state file (watcher dead) falls back to direct polling.
  - verify: three concurrent dispatches with the state dir set; `gh api rate_limit` use over 10 min is about one call a minute plus each gate's artifact download.
  - size: M

**Verification gate:** three concurrent remote gates with `NATRON_REMOTE_RUN_STATE_DIR` set all report correctly; API use is flat (≈1 call/min) while they run.

## Decisions

- 2026-11-23 — **One watcher for everything (user):** interval change first for immediate relief, then a single watcher for all runs.
- 2026-11-23 — T1 and T2 in bbe5d282f; three concurrent `--from-run` GUI dispatches sharing one watcher all passed (runs 45108088215, 45108121700, 45108152899). PR #343 in review. Flat API use to be confirmed once the old-style gates drain.
- 2026-11-23 — Merged to main as PR #343; the PM queue sets NATRON_REMOTE_RUN_STATE_DIR for every gate.
