# M94 - Delete Unused Maintainer Skill

`skills/natron-maintainer/SKILL.md` is the only file in `skills/`. M12.P1.T1
(Documentation cruft removal) deliberately fixed this file rather than
deleting it, on the theory that it's "agent-facing" guidance worth keeping
accurate. Reassessed at the user's request: nothing in this repo wires it up
for automatic discovery by an agent session — there is no `CLAUDE.md` and no
`.claude/` directory, and it did not surface in a live session's
available-skills listing — so its only path to being read is a human
manually pointing an agent at it. The user has also found at least one place
where it has gone stale again. Given no automated consumer and a repeat
drift problem, delete it rather than maintain it.

## Phase 94.1: Remove the directory

- [x] M94.P1.T1 — Delete the skills/ directory
  - files: `skills/natron-maintainer/SKILL.md` (and the now-empty `skills/`
    directory)
  - approach: `git rm -r skills/`. Before removing, grep the tracked tree
    for any reference to `skills/natron-maintainer` or `skills/` outside
    `.plan/` and `docs/decisions/` (the decision archive is allowed to cite
    deleted paths per the M12.P2.T1 precedent — it's a dated record, not a
    live reference) and confirm there is none. Record this as a
    milestone-scoped decision: it reverses M12.P1.T1's "fix, don't delete"
    call, because the premise behind keeping it — that it's read as live
    agent instruction — turned out not to hold in this repo (no
    `CLAUDE.md`, no `.claude/`, no auto-discovery).
  - verify: `test ! -e skills` (directory is gone); `git status` clean;
    `grep -rn "skills/natron-maintainer" --exclude-dir=.git --exclude-dir=.plan .`
    returns nothing.
  - size: S

**Verification gate:** `skills/` no longer exists in the tree; no tracked
file outside `.plan/` and `docs/decisions/` references
`skills/natron-maintainer`; a clean build is unaffected (no build input
depended on it).

## Decisions

- 2026-10-24 — **Delete rather than maintain `skills/natron-maintainer` (PM):** this reverses M12.P1.T1's "fix, don't delete" call. Its premise was that the file is read as live agent instruction, which doesn't hold here: there is no `CLAUDE.md`, no `.claude/` and no auto-discovery, so nothing loads it, and it had gone stale again. No tracked file outside `docs/decisions/` referenced it. Done in 7001de23b.
