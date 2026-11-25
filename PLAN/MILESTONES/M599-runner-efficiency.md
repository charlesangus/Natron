# M599 - Runner Efficiency

Cut GitHub runner slot use. The 20-concurrent-job cap is the bottleneck, not build time or cache (user-approved 2026-11-25, analysis in that day's PM session). Five changes: PR CI only for ready PRs, GUI scripts sharded into a few jobs, a ctest filter for intermediate gates, no duplicate dev-image builds, and ccache that actually hits on remote fast builds. The fifth item of the request, dropping the legacy ctest leg, is already M199.P1.T1 in M199 - Remove Legacy Scheduler and is not repeated here.

## Scope

**In:**
- `ci.yml` skipping draft PRs, GUI sharding in `remote-build.yml`, `build.sh --ctest-regex`, dev-image dedupe, ccache restore order and the remote fast-build hit rate.
- Before/after run timings and slot counts in the PR body.

**Out:**
- The legacy scheduler leg: M199 - Remove Legacy Scheduler.
- Making lane PRs open as drafts and marking them ready after a green gate. That is a `/cat-pm` workflow change, not repo code. Record it as a project decision when this milestone merges.

**Edge cases:**
- **Required checks on a draft PR:** a skipped `Build and test` must not count as passing. Confirm a draft can't be merged on a skipped check.
- **Final gate:** `--ctest-regex` is for intermediate gates only. A milestone's verification gate still runs the full suite.
- **Rebase onto M199:** M199.P1.T1 rewrites the same ctest matrix. Whichever merges second rebases onto the other.

## Phase 599.1: Fewer jobs

- [ ] M599.P1.T1 — Run PR CI only for non-draft PRs
  - files: `.github/workflows/ci.yml` (`on.pull_request` :8, every job's `if:`), `tools/ci/remote/README.md`
  - approach: set `pull_request.types: [opened, synchronize, reopened, ready_for_review]`, and give each top-level job `if: github.event_name != 'pull_request' || !github.event.pull_request.draft`. Leave push and dispatch runs unchanged. Document in the README that a draft PR gets no CI until it is marked ready.
  - verify: actionlint/`yq` parse `ci.yml`; on a test PR, a draft push starts no jobs and `gh pr ready` starts the full run.
  - size: M
- [ ] M599.P1.T2 — Shard the GUI scripts into at most 4 jobs per gate
  - files: `.github/workflows/remote-build.yml` (`gui-matrix` :662, the GUI job ~:755), `Tests/gui/run-gui-test.sh`, `tools/ci/remote/build.sh` (result collection for `--gui`)
  - approach: `gui-matrix` emits up to 4 groups, balanced by count. Each job loops over its group's scripts through `run-gui-test.sh`, with a per-script timeout, keeps going after a failure, and uploads per-script logs and screenshots under the same artifact names as now. The job fails if any script failed. `build.sh` reports per-script results from those logs.
  - verify: a gate with `--gui` on 21 scripts runs 4 GUI jobs; a deliberately failing script is reported by name while the others in its group still run; artifact names are unchanged.
  - size: L
- [ ] M599.P1.T3 — `build.sh --ctest-regex <re>` for intermediate gates
  - files: `tools/ci/remote/build.sh` (option parsing near `--repeat-test` :223, help text :9–55), `.github/workflows/remote-build.yml` (new `ctest_regex` input passed to the ctest step), `tools/ci/local/test.sh`, `tools/ci/remote/README.md`
  - approach: the option implies `--tests` and passes `-R <re>` to the ctest run. The smoke test still runs. The README says a milestone's final gate must not use it.
  - verify: `build.sh --help` lists it; a gate with `--ctest-regex NativeBlur` runs only the matching cases (check the ctest log count); without it, the full suite runs.
  - size: M

## Phase 599.2: Cache and image

- [ ] M599.P2.T1 — Build an identical dev image only once
  - files: `.github/workflows/dev-image.yml`, `.github/workflows/ci.yml` (:25), `.github/workflows/remote-build.yml` (:104)
  - approach: add a `concurrency` group keyed on the image's input hash (the tag dev-image already computes), with `cancel-in-progress: false`, so concurrent callers queue behind one build and then find the tag already published. If a job-level group can't be keyed on a computed hash, compute the hash in a cheap first job and gate the build job on "tag missing".
  - verify: two gates dispatched together after a Dockerfile change run one image build between them; with an unchanged image, both skip the build.
  - size: M
- [ ] M599.P2.T2 — Prefer main's ccache entry right after a main merge
  - files: `.github/workflows/remote-build.yml` (Restore ccache :184–192), `.github/workflows/ci.yml` (its ccache restore)
  - approach: when the branch's newest ccache entry predates the latest merge of main into the branch, restore `ccache-linux-<preset>-main-` first. Check the merge commit's date against the cache entry's date (`gh api` cache list or `actions/cache/restore` lookup-only), and fall back to the current order otherwise.
  - verify: on a branch just merged with main, the log shows the main entry restored and a higher hit rate than the prior run of that branch.
  - size: M
- [ ] M599.P2.T3 — Find and fix near-zero remote ccache hits on fast builds
  - files: `tools/ci/ccache-remote-setup.sh`, `tools/ci/ccache-remote-layout.sh`, `.github/workflows/remote-build.yml` (ccache env), `CMakePresets.json` (fast preset)
  - approach: fast builds hit 19/1313 and 0/23 remotely while debug hits 55/60. Compare `ccache -p` and the remote layout key for fast and debug: compiler flags that differ per run (`-ffile-prefix-map`, absolute paths, timestamps), `CCACHE_BASEDIR`, `hash_dir`, and a layout key that includes the preset or run id. Fix the cause in the setup script or preset.
  - verify: two consecutive fast gates on the same SHA: the second shows a remote hit rate above 90% in `ccache -s`; the debug hit rate doesn't drop.
  - size: L

**Verification gate:** actionlint passes on every changed workflow. A full remote gate with `--tests --gui` on at least 12 scripts is green. The PR body has a before/after table of job count, peak concurrent runner slots and wall time, for that gate and for one PR CI run.
