# Milestone 29: Break the link to upstream — an independent repository

`charlesangus/Natron` (`origin`) is registered on GitHub as a **fork** of
`NatronGitHub/Natron` (`gh api repos/charlesangus/Natron` returns
`"fork": true`, `"parent": {"full_name": "NatronGitHub/Natron"}`). GitHub has
no self-service API or UI action to detach a fork — only a GitHub Support
request can do that, and it can't be scripted or scheduled by this plan. The
chosen path instead: **recreate the repository as a brand-new, non-fork repo**
and cut every local and CI reference over to it. This is fully within our
control and finishes in this milestone rather than waiting on a support
ticket.

No custom Actions secrets exist beyond the default `GITHUB_TOKEN`
(`grep -rn "secrets\." .github/workflows/` finds none), and there are no
repository rulesets beyond the `main` branch protection M10 already set up
(`required_status_checks: format, lint-ci, build-and-test`) — so the only
GitHub-side state to reproduce on the new repo is that branch protection.
`has_wiki: true` but nothing confirms the wiki has content; it is **not**
migrated by this milestone (wikis are a separate git repo,
`<repo>.wiki.git`) — flag it later if it turns out to matter.

The `niikl` / `niikl-mirror` remotes (prior-art reference forks, unrelated to
`origin`) are left untouched — see Decisions.

## Phase 29.1: Stand up the independent repo

- [ ] M29.P1.T1 — Free the `Natron` name and create the new non-fork repo
  - files: none (GitHub repo operations only)
  - approach: `gh repo rename Natron-fork-archive --repo charlesangus/Natron` moves the current fork out of the way, keeping its issues/PRs/stars/network-graph history intact under the new name. Then `gh repo create charlesangus/Natron --public --description "<same description as the old repo>"` creates a brand-new repo — **not** created via GitHub's Fork button — under the now-free `Natron` name. Do not push anything into it yet.
  - verify: `gh repo view charlesangus/Natron --json isFork` reports `isFork:false` with no `parent`; `gh repo view charlesangus/Natron-fork-archive --json isFork` still reports `isFork:true` and its issue/PR count is unchanged from before the rename.
  - size: S

- [ ] M29.P1.T2 — Mirror-push all history into the new repo
  - files: none (local git remote ops)
  - approach: from this working tree, `git push --mirror https://github.com/charlesangus/Natron.git` against the freshly created repo. `--mirror` carries every branch (`main`, `RB-2.6`, `milestone/*`, ...) and tag in one shot, including the `plan` orphan branch the `.plan/` worktree tracks — do this before repointing `origin` so there's no ambiguity about which remote is the mirror target.
  - verify: `git ls-remote https://github.com/charlesangus/Natron.git` lists `main`, `RB-2.6`, and `plan`, plus the same tag set as `git tag -l` locally; `git rev-parse main` / `git rev-parse RB-2.6` match between the old fork and the new repo.
  - size: S

## Phase 29.2: Cut over

- [ ] M29.P2.T1 — Repoint local remotes and restore branch protection
  - files: none (local `.git/config` + GitHub repo settings)
  - approach: `git remote set-url origin https://github.com/charlesangus/Natron.git`. Worktrees share `.git/config`, so this also repoints `.plan/`'s `origin` — no separate step needed there, just confirm it. Recreate `main`'s required-status-checks branch protection on the new repo with the same three contexts M10 set up (`format`, `lint-ci`, `build-and-test`) — reuse the exact `gh api` call from that milestone's file/decision rather than re-deriving it.
  - verify: `git fetch origin` succeeds against the new repo; `gh api repos/charlesangus/Natron/branches/main/protection` returns `required_status_checks.contexts` = `["format","lint-ci","build-and-test"]`, matching the old repo's.
  - size: M

- [ ] M29.P2.T2 — Verify independence end-to-end and mark the old repo
  - files: none
  - approach: `gh repo view charlesangus/Natron --json isFork,parent` confirms no `fork`/`parent`. Open a throwaway PR against the new repo and confirm `Checks` and `Tests` run and branch protection blocks merge until they're green, then merge it — proving the new repo is a fully working, independent CI target, not just a copy. `gh repo edit charlesangus/Natron-fork-archive --description "Archived — superseded by charlesangus/Natron (now an independent repo, no longer a NatronGitHub fork)."`, then `gh repo archive charlesangus/Natron-fork-archive` once satisfied nothing else is needed from it.
  - verify: the throwaway PR merged cleanly through required checks on the new repo; `gh repo view charlesangus/Natron-fork-archive --json archived` reports `true`.
  - size: S

## Decisions

- 2026-09-11 — chose "recreate as a fresh non-fork repo" over filing a GitHub
  Support ticket to detach the fork. Rationale (user's call, asked directly):
  GitHub Support detachment isn't scriptable and has no ETA, while recreating
  the repo is fully scriptable with `gh`/`git` and finishes inside this
  milestone. Trade-off accepted: the old repo's stars/watchers/issue history
  stay on `Natron-fork-archive` rather than following the `Natron` name.
- 2026-09-11 — `niikl` and `niikl-mirror` remotes are left in place; they're
  local reference-only remotes to prior-art forks (see
  `[[niik-l-fork-3d-deep-cycles]]`-style notes), not part of GitHub's fork
  relationship or any CI path, so they aren't in scope for "breaking the link
  to upstream."

**Verification gate:** `gh repo view charlesangus/Natron --json isFork,parent`
shows no fork parent; the new repo carries every branch and tag the old fork
had (via the mirror push), the same `main` branch protection contexts, and a
merged PR proving CI runs end-to-end on it; the old fork is renamed,
re-described, and archived.
