# M395 - Release Notes 1.0

Write the Nucleon 1.0 release notes and automate the changelog for every later release. The notes have two parts:
- a hand-written "What's new since Natron 2.5" highlights page, drafted by agents and approved by the user;
- a full changelog page listing every merged PR since the fork, grouped by epic.

Both go on the docs site. The GitHub release body carries the highlights and a link to the changelog. From 1.1 on, the release workflow generates the grouped changelog from the previous stable tag automatically.

Context:
- **Runs after F17 - OpenNucleon Flag Day and M394 - Release Versioning** (tag scheme `vX.Y.Z`, `release.yml` creates the release).
- **Grouping source.**
  - Milestone PR titles start `M<i> - <name>` (see `git log --oneline origin/main`, and `gh pr list --state merged --limit 1000 --json number,title,mergedAt,mergeCommit`).
  - The epic for each milestone is on the plan board (`git show origin/plan:PLAN.md`, the `# Board` and `# Epics` tables). The plan branch is never merged into `main`, but it is in the same repo, so the workflow can fetch it.
  - PRs without a milestone prefix, and milestones without an epic, go under "Other".
  - Feature PRs (`F<i> - <name>`) are grouped by their members' epic, or under "Other".
- **Material for the highlights:** the `done` rows on the board, the epics' goals, `PLAN/ARCHIVE/board-context-history.md`, and the project decisions (`PLAN/DECISIONS/INDEX.md`). Claim only what is in the shipped app.
- **Docs site:** pages live under `docs/user/content/releases/` (M388 - Docs Site Scaffold reserved the nav slot).

## Scope

**In:**
- `docs/user/content/releases/1.0.md`, the highlights page. It has sections for the platform (Linux-only, Qt6, C++20, the VFX Reference Platform), native nodes, GPU compute, USD/3D, deep, colour management, Python, and the project format. It ends with a "Removed and changed since Natron" section (no Windows or macOS, no `.ntp` compatibility, renamed binaries and modules) and a "Known issues" section.
- `tools/release/changelog.py`, which takes a commit range and the plan board, and writes the grouped Markdown.
- `docs/user/content/releases/1.0-changelog.md`, generated once from the fork point to the 1.0 tag and committed.
- Automation (user, 2026-11-10): on each stable tag after 1.0, `release.yml` generates `releases/<X.Y>-changelog.md` from the previous stable tag, and puts the highlights page (if one exists for that version) plus a link to the changelog in the release body.

**Out:**
- Beta and RC release bodies, which keep GitHub's `--generate-notes`.
- Tutorials and feature guides (reference-only docs for 1.0).
- Final "Known issues" content, which is settled at M396 - Release 1.0 Cut from its UAT.

**Edge cases:**
- **Squash-merged Feature PRs** contain several milestones. The Feature PR's body lists its members (§9b), so `changelog.py` expands a Feature PR into its member milestones by parsing the `M<i> - ` lines in the body, and falls back to the Feature's own line.
- **Plan branch unavailable in CI** (fetch fails). The changelog is generated ungrouped with a warning, and the release is never blocked by it.
- **Reverted PRs.** A PR whose merge commit was later reverted is listed under "Reverted", detected by a `This reverts commit <sha>` body on `main`.
- **Very long 1.0 list** (several hundred PRs). One line per PR (title, number, link), grouped by epic, with the epic order taken from `# Epics`.

**Assumptions:**
- Unconfirmed: the fork point for the 1.0 range is the fork's first commit (`a56635515`, per `DECISIONS/2026-09-02-inherited-docs-are-not-requirements.md`).

## Phase 395.1: Changelog

- [ ] M395.P1.T1 — Write the grouped changelog generator
  - files: `tools/release/changelog.py`, `tools/release/tests/test_changelog.py`
  - approach:
    - `changelog.py --from <ref> --to <ref> --board <path-or-git-ref:path> --title <str>` lists the merged PRs in the range through `gh api` (or `git log` merge subjects as a fallback).
    - It maps `M<i>` prefixes to epics from the board and expands Feature PRs per the edge cases.
    - It writes Markdown grouped by epic in `# Epics` order, then "Other", then "Reverted".
    - Use only the standard library plus `gh`.
  - verify: Unit tests with a fixture board and fixture PR JSON cover grouping, a Feature expansion, an unprefixed PR, a revert, and a missing board. Run against the real repo from `a56635515` to `origin/main`, it produces a page that `tools/docs/build.sh` builds with `--strict`.
  - size: M

- [ ] M395.P1.T2 — Generate changelogs and release bodies in the release workflow
  - files: `.github/workflows/release.yml`, `tools/release/release-body.sh` (new)
  - approach:
    - On a stable tag only:
      - find the previous stable tag;
      - fetch `origin/plan`;
      - run `changelog.py` for the range;
      - commit `docs/user/content/releases/<X.Y>-changelog.md` to `main` through a PR opened by the workflow (never a direct push to protected `main`);
      - build the release body with `release-body.sh`, which takes the highlights page if `releases/<X.Y>.md` exists, then adds the docs-site changelog link.
    - Betas and RCs are unchanged.
  - verify: `actionlint` passes. `release-body.sh` against a scratch highlights page and changelog prints the expected body. A dry-run mode of the new step (`DRY_RUN=1`, run in a dispatched workflow on the branch against `v1.0.0` pretend-ranges) prints the PR it would open and the body without creating anything.
  - size: M

## Phase 395.2: 1.0 Notes

- [ ] M395.P2.T1 — Generate and commit the 1.0 changelog
  - files: `docs/user/content/releases/1.0-changelog.md`, `docs/user/content/releases/.pages`
  - approach: Run `changelog.py` from the fork point to `origin/main`. M396 - Release 1.0 Cut regenerates it at the tag. Commit the page and add it to the nav.
  - verify: `tools/docs/build.sh` passes `--strict`. Spot-check that five PRs land in the right epic, and list them in the PR body.
  - size: M

- [ ] M395.P2.T2 — Draft the 1.0 highlights page
  - files: `docs/user/content/releases/1.0.md`
  - approach: Agents draft the page described in Scope from the board's `done` milestones, the epic goals and the project decisions. Write for an artist or TD coming from Natron 2.5 or another compositor. Each section is two to five bullets of user-visible change. "Known issues" starts from the board's open `blocked` rows that affect users, and is finalised at M396 - Release 1.0 Cut.
  - verify: `tools/docs/build.sh` passes `--strict`. Every claim is traceable to a `done` milestone, with the mapping listed in the PR body. The rendered page is sent to the user for approval (user, 2026-11-10: agents draft, user approves).
  - size: M

**Verification gate:**
- `tools/release/tests/` passes, and `actionlint` passes.
- `tools/docs/build.sh` passes `--strict` with both release pages.
- The `release.yml` dry run prints the expected changelog PR and release body.
- The user has approved the highlights page.

## Decisions

- 2026-11-10 — The 1.0 notes are hand-written highlights plus a full changelog grouped by epic on the docs site, with the GitHub release body carrying the highlights and a link to the changelog (user).
- 2026-11-10 — Later stable releases get their grouped changelog generated automatically by the release workflow (user).
- 2026-11-10 — Epic grouping reads the plan board from `origin/plan`, and a missing board degrades to an ungrouped list rather than blocking a release: the board is the only record of which epic a milestone belongs to (planner).
