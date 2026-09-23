# Stacked milestone PRs, left open for asynchronous user checks

2026-09-22, user direction. From M34 on, the PM:
- keeps executing milestones back to back;
- packages a release AppImage at each milestone's gate;
- opens the PR and runs its review round, but leaves it open instead of merging;
- branches the next milestone off the tip of the previous milestone's branch, not off `main`, so no rebase is needed later.

Each PR's base is the previous milestone's branch, so its diff shows only that milestone. When the user merges the bottom PR (squash, delete branch), GitHub retargets the next PR onto `main`.

The user checkpoint task at each gate becomes asynchronous. The PM records it as "awaiting user check" and moves on. Findings from a check are fixed on the branch they belong to and propagated up the stack by merging each branch into the one above it, never by rebasing.

AppImages are copied to `build/appimages/<milestone-id>-<short-sha>.AppImage`, and the PR body names the file.
