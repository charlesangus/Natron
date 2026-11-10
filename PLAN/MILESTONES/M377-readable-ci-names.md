# M377 - Readable CI Names

Make every GitHub Actions workflow, run title, job, and step say in plain words what it does.
Today a PM or the user has to decode names like `dev image / dev image`, `ctest (taskgraph)`,
`gui matrix`, `remote-build`, and run titles like
`remote-build release release-ae54aeae-aeaee002`. The run title doesn't name the milestone and
shows the preset twice. The `lint-ci` job also enforces code rules, so its name is wrong too.
This milestone renames everything to sentence case, splits the code rules out of `lint-ci`, and
renames the required checks. It switches branch protection over when it merges.

## Scope

**In:**
- Required checks (user, 2026-11-09): `format` → `Format`, `lint-ci` → `Lint CI` (actionlint,
  shellcheck, Slang/glslang pin check), a new required `Code rules` job (Serialization layering
  check, kernel fma check) split out of `lint-ci`, `build-and-test` → `Build and test`.
- Workflow names: `Tests` → `Build and test`, `Checks` → `Lint`. Also `Dev image`, `Nightly`,
  `Release`, `Beta Release` and `Remote build` where their names are unclear.
- Job names in every workflow use sentence case. The pattern is `<Verb> · <what>`, e.g.
  `Build · debug`, `Test · task-graph scheduler`, `GUI · nodes_menu`, `Setup · pick test legs`,
  `Collect logs and compare renders`.
- Run titles: remote build (milestone, preset, what ran, token), nightly, release, and beta
  (version or short SHA, plus the trigger).
- Step names in every workflow say what failed when a step fails.
- A Markdown job summary (`$GITHUB_STEP_SUMMARY`) on the build, test, GUI and results jobs:
  what was built, which legs or tests failed, and the artifact name.
- Callers that match names: `tools/ci/remote/build.sh` (the token lookup in `displayTitle`,
  the `gui (<name>)` job matcher), the `beta-release.yml` `workflow_run.workflows` list,
  `CONTRIBUTING.md`, `Serialization/README.md`, and comments that name jobs.
- Switching branch protection on `main` and refreshing open PRs at merge (phase 377.5).

**Out:**
- Changing what any job builds or tests, or how jobs are split, apart from the
  `Lint CI`/`Code rules` split.
- Fixing CI flakes. Those are M378 - Scene Render Test Stability and M379 - GpuContext Test
  Segfault.
- Updating Claude memory files that cite the old check names. The planner does that after
  merge.

**Edge cases:**
- Renaming the `Tests` workflow breaks `beta-release.yml`, whose `workflow_run` trigger matches
  workflow names. Both change in the same task.
- A reusable workflow's job shows up as `<caller job> / <callee job>`. Name the caller `Dev
  image` and the callee job `Build or reuse`, so the result reads `Dev image / Build or reuse`.
- A remote build pushed to `main` (the ccache warm-up) has no token or PR. Its title is
  `Remote build · fast · main (cache warm)`.
- A branch with no open PR falls back to its branch name in the run title.
- `Code rules` must run on every PR, the same as `Lint CI`, so a required check never stays
  pending.

**Assumptions:**
- The user's `gh` token has admin rights on `charlesangus/Natron`, so it can PUT branch
  protection. Reading protection works today.
- `workflow_dispatch` has room for one more input. Remote build uses 11 of the 25 allowed.

## Phase 377.1: Required checks and workflow names

- [x] M377.P1.T1 — Split `lint-ci`, and rename the required checks and the two gate workflows
  - files: `.github/workflows/checks.yml`, `.github/workflows/ci.yml`, `.github/workflows/beta-release.yml`, `CONTRIBUTING.md`, `Serialization/README.md`
  - approach: In `checks.yml`, set the workflow `name: Lint`. Rename job `format` to `name: Format`. Rename `lint-ci` to `name: Lint CI` and keep actionlint, every shellcheck step, and the Slang/glslang pin step in it. Move the "Serialization/ layering check" and "Kernel fma check" steps, with the checkout and tool setup they need, into a new job `code-rules` with `name: Code rules`, using the same triggers and `if:` as `lint-ci`. In `ci.yml`, set `name: Build and test` and rename the aggregator job to `name: Build and test`. Update its comment, which names the required check. In `beta-release.yml`, change `workflows: ['Tests']` to `['Build and test']` and update the comment that names `format`/`lint-ci`/`build-and-test`. Update `CONTRIBUTING.md`'s merging bullet, which should list the four required checks and what each covers, and `Serialization/README.md`, which says `Code rules`. Search the rest of the repo for the old names with `git grep -nE "lint-ci|build-and-test|'Tests'|\"Tests\""` and fix every hit that names the check or the workflow.
  - verify: `actionlint .github/workflows/*.yml` is clean. The old-name `git grep` returns only history (for example `docs/decisions/2026-08-31-*`). The milestone PR's checks list shows `Format`, `Lint CI`, `Code rules` and `Build and test`.
  - size: M
- [ ] M377.P1.T2 — Record the new required-check set as a published decision
  - files: `docs/decisions/2026-11-09-ci-check-names.md` (copied from the plan at the gate, PLAN-FORMAT.md §3a)
  - approach: The plan already holds the decision file. Publish it at the gate like any project-wide decision. Don't edit `docs/decisions/2026-08-31-required-checks-name-real-jobs.md`. The new file supersedes it, and its rule (required checks name real jobs) still holds.
  - verify: the PR diff includes `docs/decisions/2026-11-09-ci-check-names.md`.
  - size: S

## Phase 377.2: Job names

- [ ] M377.P2.T1 — Rename the non-required jobs in every workflow
  - files: `.github/workflows/ci.yml`, `dev-image.yml`, `remote-build.yml`, `nightly.yml`, `release.yml`, `beta-release.yml`
  - approach: Use the sentence-case `<Verb> · <what>` pattern the user picked. Callers: `Dev image`; callee job in `dev-image.yml`: `Build or reuse`. `ci.yml`: `Build · debug`, `Build · release`, `Test · ${{ matrix.scheduler }} scheduler`, using the display word `task-graph` for `taskgraph`, through a matrix `include` that adds a `label` field. Keep `matrix.scheduler` itself unchanged, because steps and artifacts use it. `remote-build.yml`: the build job becomes `Build · ${{ env.PRESET }}`, plus ` + AppImage` when it packages one (an expression in `name:`). `ctest-legs` becomes `Setup · pick test legs`, `ctest` becomes `Test · <label> scheduler`, `gui-matrix` becomes `Setup · pick GUI scripts`, `gui` becomes `GUI · ${{ matrix.name }}`, and `results` becomes `Collect logs and compare renders`. `nightly.yml`: `Build and test · release` and `Build and test · release, GPU off`. `release.yml`: `Build and package release` and `Publish release`. `beta-release.yml`: `Tag and dispatch beta`. Leave job IDs (the YAML keys) unchanged, because `needs:` and outputs refer to them. Update the `::error::` messages and comments that quote old job names.
  - verify: `actionlint` is clean. A remote run (`tools/ci/remote/build.sh fast --tests --gui "<one script>"`) and the PR run both show the new names. In `gh run view <id> --json jobs`, no job name contains `/ dev image` or a bare YAML key.
  - size: M
- [ ] M377.P2.T2 — Point `build.sh` at the new GUI job names
  - files: `tools/ci/remote/build.sh`
  - approach: The GUI result lookup at about line 494 matches `gui (${name})`. Change it to the exact `GUI · ${name}`, using the same `·` (U+00B7) character the workflow uses. Search `tools/ci/` for any other job-name match (`ctest (`, `remote-build`, `dev image`) and update it too.
  - verify: `shellcheck tools/ci/remote/build.sh` is clean. A `--gui` remote run reports each script's pass/fail correctly in build.sh's summary. Check one passing and one deliberately failing script, or an earlier failing run via `--from-run`.
  - size: M

## Phase 377.3: Run titles

- [ ] M377.P3.T1 — Remote build run title names the milestone and what ran
  - files: `.github/workflows/remote-build.yml`, `tools/ci/remote/build.sh`
  - approach: Add a `label` input (string, default `''`, "Human label for the run title, usually the PR title"). In `build.sh`, before dispatching, set the label to `gh pr list --head "$BRANCH" --state open --json title --jq '.[0].title'`. If that is empty, use the branch name. Pass it with `-f label=…`. Shorten the token to `#<8 hex random>` and keep the SHA in the head-SHA lookup build.sh already does. Rewrite `run-name` to `Remote build · <preset> · <what ran> · <label> · <token>`. `<what ran>` is a comma list built from inputs: `tests`, `AppImage`, `<n> GUI` (or `GUI` when `gui_scripts` is set), `GUI only (from run <id>)` for `from_run`, `goldens`, and `cmake:<args>`. A push run gets `Remote build · fast · <ref_name> (cache warm)`. Build it in one `run-name` expression. If that becomes unreadable, use `format()` with nested `&&`/`||` parts and comment each part. build.sh still finds the run by `contains(displayTitle, token)`.
  - verify: `actionlint` is clean. Dispatch twice through build.sh, once with tests + GUI and once with `--from-run`, and check that both titles in `gh run list -w remote-build.yml` read as specified and that build.sh found and watched each run. A branch with no PR shows its branch name.
  - size: M
- [ ] M377.P3.T2 — Nightly, release and beta run titles
  - files: `.github/workflows/nightly.yml`, `release.yml`, `beta-release.yml`
  - approach: Add a `run-name` to each. Nightly: `Nightly · <scheduled | after merge | manual> · <ref_name>`, choosing the trigger word from `github.event_name`. Release: `Release · <inputs.tag || github.ref_name>`. Beta: `Beta · after <github.event.workflow_run.display_title>`. Workflow expressions can't shorten a SHA, and the run list already shows the commit, so titles carry no SHA. The beta tag is allocated inside the job, so the job writes it to its job summary instead.
  - verify: `actionlint` is clean. A manual `workflow_dispatch` of Nightly and the next post-merge Beta run show the new titles. Release is checked by reading the expression, since a release run publishes.
  - size: M

## Phase 377.4: Step names and job summaries

- [ ] M377.P4.T1 — Make step names say what they do
  - files: every `.github/workflows/*.yml`
  - approach: Go through every `- name:` and rename any that don't make the failure clear on their own. Example: `Run tests` becomes `Run ctest suite (<scheduler> scheduler)`, and a generic `Download` becomes `Download build tree from Build · debug`. Add `name:` to `uses:` and `run:` steps that have none. Use sentence case and no trailing periods. Don't change commands, `id:`s, or step order.
  - verify: `actionlint` is clean. In `git diff -U0`, every changed line is a `name:` line or a comment. The PR's own run shows the new step names.
  - size: M
- [ ] M377.P4.T2 — Job summaries on the run page
  - files: `.github/workflows/ci.yml`, `remote-build.yml`, `nightly.yml`, `release.yml`, `beta-release.yml`, optionally a shared `tools/ci/summary.sh`
  - approach: Append Markdown to `$GITHUB_STEP_SUMMARY` in an `if: always()` final step. Build jobs report the preset, short SHA, CMake args, and the artifact or AppImage name. Test jobs report the scheduler, the pass/fail/total counts, and the names of failed tests, read from the ctest log or JUnit output the job already writes (cap the list at 50 and link the log artifact). GUI jobs report the script, the result, and the screenshot artifact. Results / `Build and test` report one line per leg. Release/Beta report the tag. If two or more workflows share the same logic, put it in one shellcheck-clean script under `tools/ci/` and call it from the `Lint CI` shellcheck step.
  - verify: `actionlint` and `shellcheck` are clean. One remote run with tests + GUI and the PR run each show a summary per job on the run page. A run with an injected failure, such as a throwaway branch with one failing gtest or `--from-run` of a known red run, lists the failing test by name.
  - size: M

## Phase 377.5: Cutover at merge

- [ ] M377.P5.T1 — Switch branch protection and refresh open PRs
  - files: none in the repo (GitHub settings via `gh api`)
  - approach: Run this at the gate, after review and green CI, right before `gh pr merge`. 1) Save the current protection with `gh api repos/charlesangus/Natron/branches/main/protection/required_status_checks > /tmp/old-checks.json`. 2) `gh api -X PATCH repos/charlesangus/Natron/branches/main/protection/required_status_checks --input -` with `{"strict": false, "checks": [{"context":"Format"},{"context":"Lint CI"},{"context":"Code rules"},{"context":"Build and test"}]}`. 3) Merge. 4) Open PRs whose branch a running lane owns pick up the new names at their next rebase onto main (PLAN-FORMAT.md §9a). For every other open PR into `main`, meaning parked, blocked or human PRs, run `gh api -X PUT repos/charlesangus/Natron/pulls/<n>/update-branch`, and cancel its old runs. Merge main into each `feature/**` branch (no rebase, per §9b) so later Feature PRs report the new names. 5) List any open PR still missing a new required check and report it. If the merge fails, restore the saved protection.
  - verify: `gh api .../protection/required_status_checks` lists exactly the four new contexts. `gh pr list --state open` shows no PR into `main` waiting on `format`, `lint-ci` or `build-and-test`. The next merge to main triggers a Beta run, which shows the renamed `workflow_run` link working.
  - size: M

**Verification gate:** `actionlint` and `shellcheck` are clean. The PR's checks list shows exactly `Format`, `Lint CI`, `Code rules` and `Build and test` as the gate jobs, plus the renamed non-required jobs. One remote run with tests and two GUI scripts, and one `--from-run` GUI run, both carry the new run title with the milestone name, are found and watched by `build.sh`, and show the new job names, step names and job summaries. After merge, branch protection requires the four new names, a Beta run fires from the renamed `Build and test` workflow, and no open PR into `main` is waiting on an old check name. Send the user screenshots of the PR checks list and of a remote run page with its summary for approval before sign-off.

## Decisions

- 2026-11-09 — Rename the required checks rather than freeze them (user): the old names were cryptic, and `lint-ci` hid code rules. The cutover cost is accepted.
- 2026-11-09 — Split code rules out of `lint-ci` into a required `Code rules` job (user): `lint-ci` had grown to enforce codebase rules (Serialization layering, kernel fma), so its name no longer described it.
- 2026-11-09 — Sentence-case `<Verb> · <what>` job names, and `Remote build · <preset> · <what ran> · <label> · <token>` titles where the label is the PR title or the branch name (user).
- 2026-11-09 — Cut over by switching protection right before merge and refreshing PRs that no active lane owns (user): lanes already rebase after every merge to main.
- 2026-11-09 — Also in scope: workflow names, nightly/release/beta run titles, step names, and job summaries (user).
