# Milestone 4: CI/CD rebuild

`~3-4 days` · low risk. One real, working pipeline beats three broken ones.
Start this once M1 lands so every subsequent PR in M2/M3 gets gated
automatically.

## Phase 4.1: One gating pipeline

- [x] M4.P1.T1 — Single "Tests" workflow, Linux only, running in `aswf/ci-baseqt:2027.0`
  - files: `.github/workflows/ci.yml` (new/rewritten workflow)
  - approach: adapt the existing `unix_test` job from `ci.yml` — it's the one
    part of upstream's CI that's currently green — but set
    `runs-on: ubuntu-latest` with `container: aswf/ci-baseqt:2027.0`, a public,
    pre-built image with no maintenance burden of our own. Drop the
    Windows/macOS matrix legs entirely rather than disabling them.
  - verify: a pushed branch triggers the workflow and it runs inside the
    pinned container with no Windows/macOS jobs listed.
  - size: M

- [x] M4.P1.T2 — Build both debug and release
  - files: `.github/workflows/ci.yml`
  - approach: same shape as today's `unix_test` job, now against the
    ASWF-pinned VFX-Platform library set instead of Ubuntu's `apt` versions.
  - verify: workflow run shows both a debug and a release build job, both
    green.
  - size: S

- [x] M4.P1.T3 — Require it to pass before merge
  - files: GitHub repo branch protection settings (no source files)
  - approach: create the branch protection rule on the default branch, requiring
    the M4.P1.T1 workflow to pass before merge. This is the single change that
    would have caught the currently-broken upstream installer build before it
    sat red for 5 weeks. M0.P1.T1 deferred this whole task here rather than
    scaffolding it early (see M0's `## Decisions`), since there was no required
    check to point at yet.
  - verify: a PR with a failing workflow run cannot be merged via the GitHub
    UI.
  - size: S

- [x] M4.P1.T4 — Move packaging off "every push"
  - files: whichever packaging workflow remains after M0's deletions, or its
    replacement per `PLAN/DECISIONS/2026-08-29-defer-packaging-decision.md`
  - approach: upstream's `build_installer.yml` runs a full installer build on
    *every* push to any branch — expensive and how a broken build goes
    unnoticed. Trigger packaging (once decided, per the deferred packaging
    decision) on tags/releases only.
  - verify: pushing a non-tag commit does not trigger a packaging run;
    pushing a tag does.
  - size: S

- [x] M4.P1.T5 — Own the first-time-contributor approval gate
  - files: none (process commitment, not code)
  - approach: GitHub blocks Actions runs from first-time contributors pending
    manual approval (`action_required`) — every open upstream PR is currently
    stuck behind this, unreviewed. With one or two maintainers on a smaller
    fork, commit to approving/reviewing within days, not indefinitely.
  - verify: n/a — process commitment; revisit if approval latency becomes a
    problem.
  - size: S

**Verification gate:** the Tests workflow is required on the default branch,
runs debug and release inside `aswf/ci-baseqt:2027.0`, and packaging no longer
runs on every push.

## Decisions

- 2026-08-29 — T1/T2 implemented together (same file, one coherent rewrite):
  `ci.yml`'s `unix_test` job now runs `container: aswf/ci-baseqt:2027.0`,
  with the `apt`-based install step replaced by `dnf` for the handful of
  packages the image doesn't already bundle (`cairo-devel`,
  `extra-cmake-modules` via EPEL, `xorg-x11-server-Xvfb`). This sandbox has
  no Docker daemon/internet pull access, so the change could only be
  verified by static YAML/grep checks, not an actual container run — real
  verification happens on the first GitHub Actions run once pushed.
- 2026-08-29 — T4 (move packaging off "every push") has nothing to do: M0
  deleted `build_installer.yml`/`build_pacman_repo.yml` outright, and per
  `PLAN/DECISIONS/2026-08-29-defer-packaging-decision.md` no replacement
  packaging workflow exists yet. Marking not-applicable rather than
  fabricating a workflow with no packaging policy behind it — revisit once
  packaging is actually decided.
- 2026-08-29 — T5 (own the approval gate) recorded as acknowledged process
  commitment, no code involved.
- 2026-08-29 — PR #3's first CI run failed fast: `manifest for
  aswf/ci-baseqt:2027.1 not found`. `2027.1` was never actually published —
  see the corrected `PLAN/DECISIONS/2026-08-29-pin-exact-aswf-tag.md`.
  Switched to `aswf/ci-baseqt:2027.0`, the latest tag actually released per
  `gh api repos/AcademySoftwareFoundation/aswf-docker/releases`.
- 2026-08-29 — With the tag fixed, the pipeline itself works: container
  pulls, CMake configures against real Qt 6.8, and the build progresses
  through multiple targets before failing on a genuine Qt6 source bug in
  `libs/qhttpserver` (`QHttpRequest::HttpMethod` → `QChar` conversion is
  ambiguous in Qt6's moc-generated code). That's Qt6-porting work — M2's
  job, not M4's. User confirmed: merge M4 anyway (the pipeline mechanism is
  what M4 owns; M2 fixes this and other Qt6 bugs incrementally, with this
  now-working CI catching each one). Also clarified: required status
  checks (T3) gate *future* PRs' own CI runs, not a retroactive check on
  `main`'s current state, so turning it on now doesn't block anything
  already merged — it's exactly the mechanism to force M2's PRs to
  actually compile before landing.
- 2026-08-29 — T3 (require the workflow before merge) done: branch
  protection enabled on `RB-2.6` requiring the `Test Ubuntu Python 3.10`
  check (from the `Tests` workflow) to pass before merge, plus a PR
  requirement (no direct pushes). Fulfills the rule deferred from M0.P1.T1.
