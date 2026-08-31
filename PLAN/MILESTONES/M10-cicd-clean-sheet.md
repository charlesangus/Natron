# Milestone 10: Clean-sheet CI/CD

M4 and M8 got the pipeline onto the right container and reusing the same
`tools/ci/local/*.sh` scripts developers run — that part stands. What did not
survive review is the *shape*: one monolithic job named `Test Linux Python 3.13`
that builds, ctests, and smoke-tests in a single 15-minute block, plus a job
named `ci` whose entire body is `if [ "${{ needs.unix_test.result }}" !=
"success" ]; then exit 1; fi`, existing only so that renaming the real job
cannot break branch protection.

That aggregator is a workaround for a problem we own outright. This is our
fork, our default branch, and our branch-protection settings. Required checks
should name the real jobs; renaming one is a deliberate, documented
configuration change, not something the pipeline design has to be contorted to
avoid.

Clean-sheet the workflows: well-named jobs that each mean something, fast
build-independent gates that fail in seconds instead of minutes, and required
checks wired to the real job names.

**Depends on M9.** Do not start until the vendored OFX plugins are gone —
otherwise the new pipeline is designed around a "Fetch test assets" step that
M9 deletes, and every run is red for reasons unrelated to the redesign.

## Phase 10.1: Fast, build-independent gates

These need no container and no build, so they land first, run in parallel with
everything else, and give a PR a signal in seconds. They can be added while the
old `ci` aggregator is still in place — adding jobs never breaks the existing
required check.

- [ ] M10.P1.T1 — Add a `format` job that checks C++ formatting on changed files
  - files: `.github/workflows/` (new or restructured workflow file)
  - approach: `.clang-format` already exists at the repo root. Run
    `clang-format --dry-run --Werror` over the C/C++ files changed in the PR
    (not the whole tree — this is a 15-year-old codebase and a full-tree check
    would be an unbounded red wall). Use the merge-base diff to derive the file
    list. Runs on bare `ubuntu-latest`; do not pull the 12.5 GB container for
    this. Job name `format`, chosen to be worth requiring by name.
  - verify: the job passes on the milestone branch as-is, and fails on a
    deliberately misformatted file pushed to a scratch branch.
  - size: S

- [ ] M10.P1.T2 — Add a `lint-ci` job for the workflow and shell sources
  - files: `.github/workflows/` (same file as T1 or a sibling)
  - approach: run `actionlint` over `.github/workflows/*.yml` and `shellcheck`
    over `tools/ci/local/*.sh` and `.github/workflows/gen_config.sh`. Both are
    fast and need no container. Fix whatever the first run surfaces in the
    existing scripts as part of this task — a lint gate landed red is a lint
    gate everybody learns to ignore. If a finding needs a real judgement call
    rather than a mechanical fix, suppress it with an inline directive carrying
    a one-line reason.
  - verify: the job passes on the milestone branch with zero suppressions
    beyond those written in this task, and fails on a deliberately broken
    workflow file pushed to a scratch branch.
  - size: S

## Phase 10.2: Rebuild the build/test pipeline

- [ ] M10.P2.T1 — Measure the build-artifact hand-off and record the job-split shape
  - files: none (measurement + a decision entry in this file)
  - approach: splitting `build` and `test` into separate jobs requires handing
    the build tree between them, and this tree is large — the debug `Tests`
    binary alone is ~306 MB. Before committing to a shape, measure it: time
    `actions/upload-artifact` + `download-artifact` for `build/debug` against
    the wall-clock of a ccache-warm build of the same tree. Then pick, and say
    why: (a) genuinely separate `build` / `test` / `smoke` jobs with an artifact
    hand-off, if the transfer is cheap relative to the build; (b) separate jobs
    that each rebuild from the shared ccache, if that is faster than moving
    gigabytes; or (c) one `build-and-test` job with clearly named steps, if
    both hand-offs cost more than the split is worth — with `format` and
    `lint-ci` still separate, which is where the parallelism actually lives.
    Option (c) is a legitimate outcome, not a failure: the goal is a pipeline
    whose red runs point at the right stage, not job count for its own sake.
  - verify: a `## Decisions` entry in this file names the chosen shape and
    carries the measured numbers it was chosen on.
  - size: S

- [ ] M10.P2.T2 — Relax `main`'s required status checks for the redesign window
  - files: none (repository configuration)
  - approach: T3 deletes the job named `ci`, and `main` currently *requires* a
    check by that name — so the PR that removes it can never merge, and a
    required check that no job reports leaves every PR pending forever. Capture
    the current configuration first (`gh api
    repos/charlesangus/Natron/branches/main/protection > ` a scratch file, and
    paste the required-checks list into this milestone's `## Decisions`), then
    clear the required-status-checks list while keeping the
    pull-request-required and no-direct-push rules intact. This is deliberately
    reversible and scoped to the window between here and M10.P3.T1, which
    re-arms it against the new names.
  - verify: `gh api repos/charlesangus/Natron/branches/main/protection` shows an
    empty required status check list, `required_pull_request_reviews` still
    present, and `allow_force_pushes` still false.
  - size: S

- [ ] M10.P2.T3 — Rewrite `ci.yml` as named jobs, born with modern hygiene
  - files: `.github/workflows/ci.yml`
  - approach: clean-sheet the file to the shape M10.P2.T1 chose. Delete the
    `ci` aggregator job outright. Name jobs for what they do (`build`, `test`,
    `smoke`, or `build-and-test`) — never for incidental detail like the Python
    version, which is what made the old name rot. The rewrite is *born* with
    the hygiene the old file lacks rather than getting a second cleanup pass:
    a top-level `concurrency:` group keyed on the ref with
    `cancel-in-progress: true` so a second push cancels the first; explicit
    least-privilege `permissions:` (`contents: read`); and action versions
    current enough to clear the Node 20 deprecation warnings that
    `actions/checkout@v4.1.1` and `actions/cache@v4` currently emit on every
    run. Keep what M4/M7 got right: the `aswf/ci-baseqt` container, the ccache
    restore-key design (branch → main fallback) and its `if: always()` save,
    and invoking `tools/ci/local/*.sh` rather than inlining build commands.
    Preserve the substantial rationale comments on the ccache steps.
  - verify: a run on the milestone branch is green end to end with no
    deprecation annotations; the run's job list shows the new names and no `ci`
    job; a second push to the same branch cancels the first run.
  - size: L

- [ ] M10.P2.T4 — Realign `nightly.yml` with the new job structure
  - files: `.github/workflows/nightly.yml`
  - approach: nightly stays what it is — the slower release-build, release
    ctest, and smoke-test backstop that the PR gate deliberately skips (the PR
    gate stays debug-only). Bring it in line with `ci.yml`'s rewritten shape:
    same job naming convention, same concurrency and permissions blocks, same
    action versions. Its `continue-on-error` + "Check test results" pattern
    exists so the ctest log artifact uploads even on failure — keep that
    behaviour, but re-express it against the new job layout rather than copying
    the old step wiring verbatim. Drop the comments that describe the release
    build as expected-to-fail until M2 lands; M2 is done.
  - verify: a manual `workflow_dispatch` run of Nightly on the milestone branch
    is green, uploads the ctest log artifact, and emits no deprecation
    annotations.
  - size: M

## Phase 10.3: Re-arm the gate

- [ ] M10.P3.T1 — Wire branch protection to the real job names
  - files: none (repository configuration)
  - approach: once the rewritten workflows have merged to `main` and reported
    at least once, set `main`'s required status checks to the actual job names
    the new pipeline reports — `format`, `lint-ci`, and whichever
    build/test job(s) M10.P2.T1 settled on — restoring the rest of the
    protection captured in M10.P2.T2. Record the final list, and the fact that
    renaming a job now requires updating this list, in a project-wide decision
    file so nobody reintroduces an aggregator to avoid it.
  - verify: a scratch PR against `main` shows exactly the intended required
    checks, all reporting; and a PR that fails `format` alone is blocked from
    merging.
  - size: S

- [ ] M10.P3.T2 — Resolve the `aswf/ci-baseqt` tag across CI and the dev shell
  - files: `.github/workflows/ci.yml`, `.github/workflows/nightly.yml`,
    `tools/ci/local/Dockerfile`, `tools/ci/local/devshell.sh`,
    `tools/ci/local/README.md`
  - approach: closes the board's standing open question. The workflows pin
    `2027.0` while `DECISIONS/2026-08-29-pin-exact-aswf-tag.md` records `2027.1`
    as the chosen tag; M7 matched `ci.yml` at `2027.0` so the local shell
    reproduces CI exactly. M2 is done and the smoke-test diagnosis that was
    blocking this is finished, so pick one tag and make every reference agree.
    Verify the chosen image actually still exists and carries GCC 14.2 before
    committing to it. Then supersede or confirm the existing decision file, and
    remove the question from the board.
  - verify: `grep -rn "ci-baseqt:" .github/ tools/` shows a single tag
    everywhere; a full CI run and a local `devshell.sh` build both succeed on it.
  - size: S

**Verification gate:** a PR against `main` reports the new named jobs and no
`ci` aggregator; every required status check in branch protection corresponds
to a job that actually runs; `format` and `lint-ci` return a verdict in under a
minute; a full green run emits zero deprecation annotations; a superseding push
cancels the in-flight run; and Nightly is green on `workflow_dispatch`.
