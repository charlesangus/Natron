# M269 - Remote GUI Checks

Run the Xvfb GUI check scripts (Tests/gui/*.py) on GitHub Actions instead of the single-tenant local natron-dev container, one job per script in parallel, with screenshots and logs uploaded as artifacts.

## Scope

- In: a `gui` job in `.github/workflows/remote-build.yml` (matrix over a `gui_scripts` input) that runs `Tests/gui/run-gui-test.sh` against the run's AppImage; a `from_run` input that skips the build and uses an earlier run's AppImage; `tools/ci/remote/build.sh --gui "<scripts>" [--from-run <id>]` that dispatches, waits and downloads the per-script outputs into `build/remote/<preset>/gui/<script>/`.
- Out: benches (runners are too noisy to time; they stay local); GPU GUI checks.
- Edge cases: the AppImage of a branch that predates M269 (from_run works for any run that uploaded `natron-<preset>-<sha>`); a script that times out (124) fails only its own job; screen size 1920x1080.
- Assumptions: the published natron-dev image runs run-gui-test.sh as the local container does (same /etc/ld.so.preload layout); the workflow has `actions: read` to download another run's artifact.

## Phase 269.1

- [x] M269.P1.T1 — GUI job and inputs in remote-build.yml
  - files: .github/workflows/remote-build.yml
  - approach: inputs `gui_scripts` (space-separated paths under Tests/gui) and `from_run` (run id); build job skipped when from_run is set; `gui` job matrix from the script list, downloads the AppImage (this run or from_run via run-id + github-token), runs run-gui-test.sh with GUI_TEST_SCREEN_SIZE=1920x1080, uploads NATRON_GUI_TEST_OUT as `gui-<name>-<sha>` even on failure.
  - verify: dispatch on the M269 branch with from_run = a green release run and one existing script.
  - size: L
- [x] M269.P1.T2 — build.sh --gui / --from-run
  - files: tools/ci/remote/build.sh, tools/ci/remote/README.md
  - approach: pass the inputs, download every gui-* artifact, print a per-script pass/fail summary.
  - verify: `tools/ci/remote/build.sh release --from-run <id> --gui "Tests/gui/m55_node_graph.py"` downloads screenshots.
  - size: M

## Decisions

- 2026-10-27: first dispatch (run 40625390115, from M215's run 40620856879) ran 3 scripts in parallel in ~4 min; Xvfb/GL works on hosted runners (m215 87/87, m55 148/148; m214 1 real failure). Code d6762db96. The GUI backlog of 17 milestones is dispatched from this branch with from_run.
- 2026-10-27: user asked to run the container-queue GUI work on Actions in parallel instead of waiting on the local container. Benches stay local.

Shipped 2026-10-28: PR #141, squash 932c072c9.
