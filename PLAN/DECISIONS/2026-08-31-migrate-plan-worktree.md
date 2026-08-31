# Move the plan into the `.plan/` worktree

The plan moved out of the code repo's branches and onto an orphan `plan`
branch, checked out at `.plan/` and excluded via `.git/info/exclude`
(PLAN-FORMAT.md §1a). Board churn, the inbox, and the archive no longer land in
code commits or PR diffs, and a board edited by every milestone branch stops
being a standing merge conflict.

This supersedes `2026-08-31-defer-plan-worktree-migration.md`, which held the
move until M2 merged. The trigger for going early: M2's work is complete and
its remaining CI failure turned out to be an inherited vendored-plugin problem
(M9), not Qt6 scope — so "after M2" was no longer a meaningful boundary, and
M9/M10 are about to add three milestones' worth of plan churn that there is no
reason to route through the code history.

The plan's past history stays in the code repo's log, unrewritten. The seam is
two commits: `bb473bbbe` on the `plan` branch (import), and the matching
removal on `milestone/m2-qt6-migration`, whose message names it.
