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

**M9 is cancelled; this milestone is unblocked.** The dependency it recorded
has inverted: "Fetch test assets" is no longer a step to design around the
removal of — it now builds the OFX plugin bundle from pinned source, and
`BaseTest`'s 28 green ctest cases depend on it
(`DECISIONS/2026-08-31-restore-vendored-ofx-plugin-tests.md`). Preserve that
step and its companion `Cache test assets` through the rewrite.

## Phase 10.1: Fast, build-independent gates

These need no container and no build, so they land first, run in parallel with
everything else, and give a PR a signal in seconds. They can be added while the
old `ci` aggregator is still in place — adding jobs never breaks the existing
required check.

- [x] M10.P1.T1 — Add a `format` job that checks C++ formatting on changed files
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

- [x] M10.P1.T2 — Add a `lint-ci` job for the workflow and shell sources
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

- [x] M10.P2.T1 — Measure the build-artifact hand-off and record the job-split shape
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

- [x] M10.P2.T2 — Relax `main`'s required status checks for the redesign window
  - files: none (repository configuration)
  - approach: T3 deletes the job named `ci`, and `main` currently *requires* a
    check by that name — so the PR that removes it can never merge, and a
    required check that no job reports leaves every PR pending forever. Capture
    the current configuration first (`gh api
    repos/charlesangus/Natron/branches/main/protection > ` a scratch file, and
    paste the required-checks list into this milestone's `## Decisions`), then
    clear the required-status-checks list while leaving every other rule
    untouched. This is deliberately reversible and scoped to the window between
    here and M10.P3.T1, which re-arms it against the new names — so do it as
    late as possible, immediately before the merge that needs it, not on
    schedule. Note the protection is thinner than this task assumed when it was
    written: there is **no** review requirement and no push restriction on
    `main` today, so "keep the pull-request-required rule intact" describes a
    rule that does not exist. Do not add one here; that is a separate call.
  - verify: `gh api repos/charlesangus/Natron/branches/main/protection` shows an
    empty required status check list, and `allow_force_pushes`,
    `allow_deletions` and `lock_branch` all still false.
  - size: S

- [x] M10.P2.T3 — Rewrite `ci.yml` as named jobs, born with modern hygiene
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
    run. Keep what M4/M7/M2 got right: the `aswf/ci-vfxall:2027-clang21.1`
    container, the ccache restore-key design (branch → main fallback) and its
    `if: always()` save, the `Cache test assets` / `Fetch test assets` pair
    that builds the OFX plugin bundle from source, and invoking
    `tools/ci/local/*.sh` rather than inlining build commands. Preserve the
    substantial rationale comments on the ccache and test-asset steps, minus
    the ccache comment's now-stale "main doesn't compile under Qt6 until M2
    lands" justification — restate why `if: always()` still matters without
    tying it to a milestone that has shipped.
    **Also drop `paths-ignore` from both triggers.** You cannot require a job
    by exact name and simultaneously let the workflow's top-level trigger skip
    it for certain paths: a workflow skipped by path filtering reports no check
    at all, and a required check that never reports leaves the PR pending
    forever (GitHub's own docs say so and advise against requiring skippable
    workflows). It is currently inert only by typo — bare `Documentation` is
    one path segment and never matches `Documentation/anything` — so the
    hazard arrives the moment someone "corrects" it to `Documentation/**`. If
    skipping the container build for docs-only changes is still wanted, it has
    to be an in-job conditional so the required job name still posts a real
    conclusion.
  - verify: a run on the milestone branch is green end to end with no
    deprecation annotations; the run's job list shows the new names and no `ci`
    job; a second push to the same branch cancels the first run; and
    `grep -n paths-ignore .github/workflows/ci.yml` returns nothing.
  - size: L

- [x] M10.P2.T4 — Realign `nightly.yml` with the new job structure
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

- [x] M10.P3.T1 — Wire branch protection to the real job names
  - files: none (repository configuration)
  - approach: once the rewritten workflows have merged to `main` and reported
    at least once, set `main`'s required status checks to the actual job names
    the new pipeline reports — `format`, `lint-ci`, and whichever
    build/test job(s) M10.P2.T1 settled on — restoring the rest of the
    protection captured in M10.P2.T2. Record the final list, and the fact that
    renaming a job now requires updating this list, in a project-wide decision
    file so nobody reintroduces an aggregator to avoid it. Two documentation
    loose ends belong here too: `CONTRIBUTING.md` (around lines 120-123) still
    says "one required status check must pass", which is now three; and the
    published `docs/decisions/` copies keep the old job name as historical
    narrative, which is correct and must be left alone.
  - verify: a scratch PR against `main` shows exactly the intended required
    checks, all reporting; and a PR that fails `format` alone is blocked from
    merging.
  - size: S

- [x] M10.P3.T2 — Close out the container-image tag question
  - files: `.github/workflows/ci.yml`, `.github/workflows/nightly.yml`,
    `tools/ci/local/Dockerfile`, `tools/ci/local/README.md`
  - approach: closes the board's standing open question. The drift it recorded
    (`ci-baseqt:2027.0` in the workflows vs. `2027.1` in
    `DECISIONS/2026-08-29-pin-exact-aswf-tag.md`) is already gone: M2's image
    switch moved every reference to `aswf/ci-vfxall:2027-clang21.1`, and
    `2026-08-31-switch-ci-image-to-vfxall.md` supersedes the older pin. What is
    left is confirmation and bookkeeping — re-run the grep after the P2
    rewrites to prove they did not reintroduce a second tag, check the pinned
    tag is still published, and delete the trailing paragraph under the board's
    `# Open questions`, leaving only `_None awaiting a human answer._`.
  - verify: `grep -rn "ci-baseqt:\|ci-vfxall:" .github/ tools/` shows exactly
    one image tag everywhere; the board carries no open question.
  - size: S

**Verification gate:** a PR against `main` reports the new named jobs and no
`ci` aggregator; every required status check in branch protection corresponds
to a job that actually runs; `format` and `lint-ci` return a verdict in under a
minute; a full green run emits zero deprecation annotations; a superseding push
cancels the in-flight run; and Nightly is green on `workflow_dispatch`.

## Decisions

- 2026-08-31 — freshness check at promotion (PLAN-FORMAT.md §5a) re-planned two
  tasks and the milestone preamble; the rest passed unchanged. `M10.P2.T3` now
  names `aswf/ci-vfxall:2027-clang21.1` (M2 switched images) and must preserve
  the `Cache test assets` / `Fetch test assets` pair, which builds the OFX
  plugin bundle from source rather than being M9's deletion target.
  `M10.P3.T2` shrank to confirmation-and-bookkeeping: the tag drift it was
  written to resolve no longer exists, since the image switch made every
  reference agree on one tag.

- 2026-08-31 — `checks.yml` carries no `paths-ignore`, and `M10.P2.T3` removes
  `ci.yml`'s. Consulted after the T1 reviewer added a path filter to match
  `ci.yml` and the implementer had deliberately omitted one. A workflow skipped
  by path filtering posts no check run, and a required check that never reports
  blocks the PR indefinitely — unlike a skipped *job*, which reports a
  `skipped` conclusion that branch protection accepts. Since `M10.P3.T1`
  requires these jobs by literal name, no trigger-level path filter can stay.
  The existing filter is inert only because bare `Documentation` never matches
  a path inside `Documentation/`, which makes it a trap rather than a
  safeguard.

- 2026-08-31 — `main`'s branch protection as captured before any M10 change
  (`gh api repos/charlesangus/Natron/branches/main/protection`):
  `required_status_checks.contexts: ["ci"]` with `strict: false`;
  `required_pull_request_reviews: null`; `enforce_admins: false`;
  `allow_force_pushes: false`; `allow_deletions: false`;
  `required_linear_history: false`; `required_conversation_resolution: false`;
  `lock_branch: false`; no push restrictions. `M10.P3.T1` restores from this,
  replacing only the `contexts` list. Note there is no review requirement to
  preserve — `M10.P2.T2` was written assuming one existed.

- 2026-08-31 — **`ci.yml` keeps one `build-and-test` job with clearly named
  steps (option (c))**, measured rather than assumed. One two-job experiment on
  a scratch branch (run 33448187599, since deleted) against the 220s warm
  monolithic baseline:

  | Quantity | Measured |
  |---|---|
  | ccache-warm debug build | 66.8s, 548/549 compile hits (99.8%) |
  | `build/debug` on disk | 4.1 GB, 1112 files |
  | `upload-artifact` of it | 95s → 935 MiB stored (4.4:1) |
  | `download-artifact` of it | 20.0s (~47 MB/s) |
  | Container init, per job | 84s and 134s |

  The decisive number is not the transfer, it is the **container init: every
  additional job pays 1–2 minutes to pull `aswf/ci-vfxall` before it does any
  work.** A two-job split therefore costs 150–250s (upload+download+init) or
  130–190s (rebuild-from-ccache+init) on every run, against a *total* warm
  runtime of 220s. Rebuilding from ccache (67s) does beat moving the tree
  (115s), so (b) is the better hand-off if one is ever needed — but both are
  pure overhead here.

  Splitting also buys less than it appears: build and test are inherently
  sequential, so no parallelism is unlocked, and per-stage red/green already
  exists — named steps report their own pass/fail and duration in the Actions
  UI. The genuine parallelism is `format` and `lint-ci`, which are already
  separate jobs on a bare runner.

  If a split is ever revisited: ~75% of the 4.1 GB is `.o`/`.a` intermediates
  already linked into the three binaries ctest actually runs (~1.02 GB), so a
  path-scoped artifact would cut the transfer 3–4x. That still would not
  overcome the per-job container init.

- 2026-08-31 — `M10.P2.T2` executed as a **repoint, not a relaxation**.
  The task planned to clear `main`'s required-status-check list for the
  redesign window and re-arm it in `M10.P3.T1`. Instead the list went straight
  from `["ci"]` to `["format", "lint-ci", "build-and-test"]` in one call, at
  the moment the `ci.yml` rewrite was pushed. Same unblocking effect — the PR
  deleting the `ci` job can now merge — with no window in which `main` has no
  required check at all. It is safe because branch protection evaluates the
  checks a PR actually reports, not the workflows on `main`: PR #7 reports all
  three. The known cost is that a PR from a branch predating `checks.yml` would
  report none of the three and would strand; that is the intended end state
  anyway, and there are no such branches in flight. `M10.P3.T1` is now
  confirmation and documentation rather than configuration.

- 2026-08-31 — follow-ups found while reviewing the `ci.yml` rewrite, both
  pre-existing and deliberately out of scope. `Cache test assets` is a plain
  `actions/cache`, whose `post-if: success()` means a red build discards the
  ~52 MB asset cache and rebuilds the OFX bundle next run — the same trap that
  `Save ccache` was split into restore/save with `if: always()` to avoid. And
  `Save ccache` runs with an empty `key` if `Checkout branch` ever fails, since
  `Restore ccache` is then skipped and its `cache-primary-key` output is empty;
  it only adds a second red step to an already-red job.

- 2026-08-31 — `M10.P3.T2` closed as confirmation only. Every reference in
  `.github/` and `tools/` already resolves to the single tag
  `aswf/ci-vfxall:2027-clang21.1`, and the P2 rewrites did not reintroduce a
  second one. The one surviving `ci-baseqt` mention is a line in
  `tools/ci/local/Dockerfile` recording which package statuses were carried
  over from the old image — historical provenance, not a live pin, and correct
  to keep. The board's standing open question is removed.

- 2026-08-31 — gate evidence. Concurrency: push `d9989c4d5` cancelled the
  in-flight `Tests` run `33450556771` for the same ref, so
  `cancel-in-progress` works as intended. Enforcement: a throwaway PR (#8,
  since closed and its branch deleted) carrying one deliberately misformatted
  C++ file turned `format` red in 16s while `lint-ci` stayed green, and GitHub
  reported the PR as `BLOCKED` — the required checks are enforced, not merely
  listed.
