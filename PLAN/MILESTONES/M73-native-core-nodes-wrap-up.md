# M73 - Native Core Nodes Wrap-Up

Full title: Housekeeping to close out M67 - Native Core Nodes

Parcel 2 merged on 2026-10-08, but M67 - Native Core Nodes left loose ends: `OPENFX_MISC_REF` pins a branch commit of an unmerged fork PR, and the parcel-1/2 stacked branches and worktrees are still lying around. This milestone closes them so the next milestones start from a clean `main`. No feature work.

## Phase 73.1: Re-pin and clean up

- [x] M73.P1.T1 — Merge openfx-misc#7 and re-pin `OPENFX_MISC_REF`
  - files: `tools/ci/local/fetch-assets.sh` (and any workflow or doc that names the old pin; grep for `59ae4c26a` and `OPENFX_MISC_REF`)
  - approach:
    - Check `gh pr view 7 --repo charlesangus/openfx-misc` is green, then merge it with a merge commit (the convention used for the fork PRs in parcels 1 and 2). Do not squash: the pin must be an ancestor-stable commit.
    - Branch `milestone/m73-native-core-nodes-wrap-up` off `main`. Set `OPENFX_MISC_REF` to the merge commit and check no other file references the branch commit.
    - Run the full-tree `git clang-format` against the merge base before pushing. Open the PR against `main`; wait for CI.
  - verify: `fetch-assets.sh` in a clean `build/assets` resolves and builds the plugin bundle at the new pin; CI (`format`, `lint-ci`, `build-and-test`) green.
  - size: M

- [x] M73.P1.T2 — Remove merged stack branches and worktrees
  - files: none in the repo; local and `origin` branches `milestone/m6*`, `milestone/m37*`, `milestone/m50*`, worktrees under `build/wt/`
  - approach: for each candidate, confirm it is merged (`git branch --merged main`, or its PR is merged per `gh pr view`; squash merges need the PR check) before `git worktree remove` and `git branch -d`/`git push origin --delete`. Leave anything unmerged or carrying unpushed commits and list it in the report. Never remove `build/appimages/`.
  - verify: `git worktree list` and `git branch -a` show only unmerged/active branches; the report lists everything skipped and why.
  - size: M

- [ ] M73.P1.T3 — Drop the resolved open question
  - files: `PLAN.md` (PM work)
  - approach: after T1 merges, remove the M67 `OPENFX_MISC_REF` item from `# Open questions` and record the merge commit in this milestone's `## Decisions`.
  - verify: board has no stale mention of the branch pin.
  - size: S

**Verification gate:** `main` carries a pin that is a merge commit on openfx-misc's default branch; CI green; no merged stack branches or worktrees remain.

## Decisions
- 2026-10-08 — **Scheduled first (user):** runs before the M64 re-evaluation and the backlog. The unconfirmed ChromaKeyer NaN (needs the user's exact settings) is not part of this milestone.
- 2026-10-08 — **Freshness check:** the live pin is `59ae4c26a` (openfx-misc#7's head), not `3060fe33b`; T1's grep updated. T2 resized S → M: deciding whether a squash-merged branch is safe to delete needs judgement.
- 2026-10-08 — **openfx-misc#7 merged** as `9fb0d0904` (merge commit; tree `ecaf615f` identical to the old pin `59ae4c26a`). The fork has no CI of its own; Natron's CI cold-fetching the new ref on PR #43 stands in for the local clean `build/assets` fetch, since the source is byte-identical.
- 2026-10-08 — **Cleanup done:** 13 local + 7 remote merged milestone branches deleted (each checked against its squash-merged PR's head), plus `milestone/m67-native-core-nodes`. Worktrees `build/wt/m30`, `m64`, `m67-base` removed (`m64`/`m67-base` needed `--force` only because they hold submodules; both clean). `build/wt/m64/build/bench` (M64 start-of-milestone benchmarks and ablations, not in the main checkout) was copied to `build/bench/from-wt-m64/` first. Seven fully-pushed, merged fork clones under `build/wt/` deleted. Kept: `milestone/m64-tiled-rendering`, `ci-smoke-test-m2p3t1a`, `m7-validation`, upstream-era `origin/*` branches, and `build/wt/m65-supportext` — openfx-supportext#4 is still open (see Open questions).
