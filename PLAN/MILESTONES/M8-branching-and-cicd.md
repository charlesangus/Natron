# Milestone 8: Branching model and CI/CD rebuild

`~2-3 days` · unblocks M2's merge path. M4 delivered *a* pipeline; this
delivers one that fits how this fork actually works. Three things forced it:

1. **Branch protection is keyed to a CI job's display name.** Renaming the
   stale `Test Ubuntu Python 3.10` job in `M2.P3.T1b` broke the required check
   outright. A required check must be a stable contract, not a label someone
   might reasonably edit.
2. **Every PR pays for two cold builds.** No caching, one sequential job doing
   debug build → ctest → smoke → release build → ctest. This is the same cost
   that made M7 necessary; M7 fixed it locally and left CI untouched.
3. **The default branch is `RB-2.6`** — upstream's release-branch naming,
   inherited along with ~25 branches mostly dead since 2021-2024. What we
   actually run is trunk-based development against a branch that is named as
   though it were a maintenance line.

**The central design choice:** CI invokes `tools/ci/local/build.sh` and
`tools/ci/local/test.sh` rather than restating their steps in YAML. One code
path means local and CI *cannot* drift, which converts M7's fidelity guarantee
from documentation into structure. Everything else here is in service of that.

**Constraint on this milestone's own gate — read before starting.** `RB-2.6`
does not compile under Qt6 and has been red since M1; M2 owns those fixes. So a
CI pipeline built here will correctly report failure, and **this milestone
cannot be verified by a green run.** Its gate is that the pipeline *behaves
correctly*: the right jobs run, the cache restores, failures surface with usable
logs, and the required check reports under a stable name. Green arrives with M2.
Do not chase a green build here, and do not weaken the gate to manufacture one.

## Phase 8.1: Trunk-based branching

- [x] M8.P1.T1 — Rename the default branch to `main`
  - files: GitHub repo settings, `.github/workflows/ci.yml` (trigger branches)
  - approach: use GitHub's branch rename so it retains open PRs and redirects
    the old ref. Keep `RB-2.6` as a frozen pointer at the rename commit so
    existing links and the upstream bridge still resolve — do not delete it.
    Update the workflow's `push`/`pull_request` trigger lists, which currently
    name `RB-2.5` and `RB-2.6`, to `main`. Leave the ~25 legacy `RB-*` and
    experiment branches untouched: they cost nothing, they are the historical
    record, and deleting them would break the upstream bridge recorded in
    `PLAN/DECISIONS/2026-08-29-upstream-agnostic-fixes.md`.
  - verify: `gh repo view --json defaultBranchRef` reports `main`; `git fetch`
    then `git log origin/main -1` matches the old `RB-2.6` tip; the workflow's
    triggers name `main`; local clones can still resolve `RB-2.6`.
  - size: S

- [x] M8.P1.T2 — Document the branching model and wire the upstream remote
  - files: `CONTRIBUTING.md` (or a new `docs/development.md` — check which
    exists first), `.git` remote config is local-only so document rather than
    script it
  - approach: write down what we actually do, briefly: `main` is trunk;
    short-lived `milestone/*` and `fix/*` branches; PR with squash merge; one
    required check. Record that `NatronGitHub/Natron` is the upstream bridge
    and how to add it (`git remote add upstream ...`), per the
    upstream-agnostic-fixes decision. Note that the legacy `RB-*` branches are
    kept deliberately and are not merge targets. Keep it short — this is
    orientation for a new contributor, not policy.
  - verify: a reader can determine, from this file alone, which branch to
    branch from, what to name their branch, and what must pass before merge.
  - size: S

## Phase 8.2: A pipeline that reuses the local loop

- [x] M8.P2.T1 — Make `build.sh` and `test.sh` usable from inside CI
  - files: `tools/ci/local/build.sh`, `tools/ci/local/test.sh`,
    `tools/ci/local/devshell.sh`
  - approach: both scripts currently re-exec themselves through `devshell.sh`.
    In CI the job *already runs inside* `aswf/ci-baseqt:2027.0`, so they must
    detect that and run directly instead of trying to nest a container. They
    already guard on `CI=True`; verify that guard actually holds under GitHub
    Actions (which sets `CI=true`, lowercase) rather than assuming it — a
    case-sensitive comparison here would silently attempt to nest Docker. Add
    an explicit override (e.g. `NATRON_IN_CONTAINER=1`) rather than relying
    solely on inference. Also allow the parallelism to be set rather than
    hardcoded to `nproc`, since runners and this machine differ.
  - verify: `NATRON_IN_CONTAINER=1 tools/ci/local/build.sh` run from inside
    `devshell.sh` builds without attempting to start a container; the normal
    host invocation still works unchanged; `CI=true` (lowercase) is handled.
  - size: M

- [x] M8.P2.T2 — Rewrite the workflow: fast PR gate calling the shared scripts
  - files: `.github/workflows/ci.yml`
  - approach: PR gate is configure + debug build + `ctest`, and nothing else.
    Steps call `tools/ci/local/build.sh` and `tools/ci/local/test.sh` instead
    of restating cmake/ctest invocations. Keep the container image pinned to
    the same tag. Fetch assets via `tools/ci/local/fetch-assets.sh` so that
    path is shared too. Drop the release build and the smoke test from the gate
    — they move to M8.P2.T4. Delete the `ci(temp)` diagnostic steps
    (`::error::` annotation dump, the `::notice::` outcome reporter) that were
    added for `M2.P3.T1a`; the local loop replaces what they were for. This
    supersedes `M2.P3.T3`, which should be struck when this lands.
  - verify: a pushed branch triggers the workflow; it runs the shared scripts;
    the debug build and ctest steps execute and report; no release build runs
    on a PR. Expect the build itself to FAIL (RB-2.6 is red) — what is being
    verified is that the pipeline runs correctly and reports that failure
    clearly.
  - size: M

- [ ] M8.P2.T3 — Cache ccache between runs
  - files: `.github/workflows/ci.yml`
  - approach: restore/save `CCACHE_DIR` with `actions/cache`, keyed so that a
    PR reuses the base branch's cache and a merge to `main` refreshes it.
    Export `CCACHE_MAXSIZE` explicitly — M7 found ccache thrashing at its 5 GiB
    default (193 cleanups, 0.18% hit rate), and a runner cache should be sized
    for the runner, not copied from the local 40G. Print `ccache -s` at the end
    of the job so the hit rate is visible and regressions in it are noticeable.
  - verify: two consecutive runs on the same branch — the second restores the
    cache and reports a materially higher hit rate than the first. Report both
    numbers.
  - size: M

- [ ] M8.P2.T4 — Post-merge and nightly: release build, smoke test, artifacts
  - files: `.github/workflows/` (new workflow file)
  - approach: on push to `main` and on a nightly schedule, run the fuller set —
    release build, `ctest`, and the Python smoke test. Upload test logs with
    `if: always()`, and fix the existing bug where the artifact step points at
    `release/Testing/...` with no `always()` guard, so logs are lost precisely
    when the build fails. Name artifacts accurately (the current one says
    "Ubuntu").
  - verify: the workflow appears under Actions with a schedule; a manual
    `workflow_dispatch` run executes the release path; log artifacts are
    uploaded even when a step fails.
  - size: M

## Phase 8.3: Make the gate stable

- [ ] M8.P3.T1 — A stable required check that job renames cannot break
  - files: `.github/workflows/ci.yml`, GitHub branch protection settings
  - approach: add a final aggregator job (e.g. `name: ci`) that `needs:` the
    real jobs and fails if any dependency failed — including the skipped and
    cancelled cases, which a naive `if: success()` misses. Point `main`'s
    branch protection at that one stable context and remove the old
    `Test Ubuntu Python 3.10` requirement. This is the fix for the failure that
    prompted this milestone: inner job names become free to change.
  - verify: branch protection on `main` requires exactly the aggregator
    context; a PR with a failing inner job shows the aggregator failing and
    cannot be merged; renaming an inner job does not change the required
    context.
  - size: M

**Verification gate:** the default branch is `main` with the legacy refs intact;
CI runs `tools/ci/local/build.sh` and `test.sh` rather than duplicating them;
a PR runs only the fast gate and a second run demonstrably reuses the ccache;
release/smoke run post-merge with logs uploaded on failure; and branch
protection requires a single stable check that an inner job rename does not
break. The build itself is still expected to fail until M2 lands — the gate is
that the pipeline behaves correctly, not that it is green.

## Decisions

- 2026-08-30 — **M8 takes ownership of `.github/workflows/ci.yml`; M2's
  `M2.P3.T1b` edit to it becomes redundant.** `M2.P3.T1b` corrected
  `PYTHON_VERSION` to `3.13` and renamed the job, but it landed on
  `ci-smoke-test-m2p3t1a`, and M8 branches off `main`, which does not have it.
  Rather than leave the two to collide at merge, M8's rewrite carries the same
  correction forward. When M2 merges, expect a conflict in this file and
  resolve it in M8's favour for everything except the Python-bindings smoke
  test step, which is M2's and moves to the post-merge workflow (`M8.P2.T4`).
- 2026-08-30 — the workflow's `dnf install` step is dropped, not ported.
  `tools/ci/local/Dockerfile`'s drift audit establishes that
  `aswf/ci-baseqt:2027.0` already ships every package it installed, except
  `extra-cmake-modules`, whose absence only disables Wayland support and is
  already recorded as a known divergence in M7.
- 2026-08-30 — `xvfb-run` moves out of the job's default shell. It was set as
  `defaults.run.shell: xvfb-run --auto-servernum bash -l {0}`, wrapping every
  step, while `test.sh` independently wraps its own `ctest` call the same way —
  so the gate would have nested one Xvfb server inside another. `test.sh` is
  now the sole owner of the wrap, and only the step that needs a display gets
  one.
