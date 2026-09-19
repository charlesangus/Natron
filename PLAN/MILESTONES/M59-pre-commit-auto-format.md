# Milestone 59: Pre-commit hook that auto-formats staged C/C++ so PRs stop failing CI's `format` check

Small housekeeping milestone. CI's `format` job (`.github/workflows/checks.yml`)
runs `git clang-format` (pinned `clang-format==21.1.8`) on changed lines, and PRs
keep failing it because nothing runs the same check before the commit exists.
A `.git-hooks/pre-commit` already mirrors the CI check, but it is (a) check-only
— it prints a diff and tells you to fix it, (b) not installed anywhere
(`.git/hooks/` is empty, no `core.hooksPath`), and (c) advisory: when
clang-format isn't on the machine it warns and lets the commit through, which
on this host (no `clang_format` module, no `clang-format` on PATH) is the
default outcome. Outcome of this milestone: a committed C/C++ change is
already formatted the way CI wants, or the commit does not happen.

Scope cuts: no whole-tree reformat, no new CI job, no change to the CI
`format` step itself, no change to the `post-commit`/`git-version` hook.

## Phase 59.1: Hook, installer, docs

- [x] M59.P1.T1 — Make `.git-hooks/pre-commit` auto-format staged changes and hard-fail on a missing pinned tool
  - files: `.git-hooks/pre-commit`
  - approach: Keep the existing structure and binary resolution (pinned pip `clang_format` module first — `pip install --user --break-system-packages clang-format==21.1.8` is the known-working host install — then PATH fallback). Change three behaviours: (1) a missing `python3`/`clang_format`/`git-clang-format` is now `exit 1` with the exact install line, not `warn_and_pass` — delete that function; (2) replace the `--staged --diff` check with an apply: run `git clang-format --binary "$CF_BINARY" --extensions "$EXTENSIONS" --staged -- . ':(exclude)Documentation/templates/'` so fixes land in the index and working tree, then `git add` the files it reports as modified so the commit contains the formatted version, print what was reformatted, and exit 0; (3) mirror CI's `':(exclude)Documentation/templates/'` pathspec (the current hook lacks it). Verify `--staged` apply semantics against the pinned 21.1.8 before relying on them — in particular, when a file has both staged and unstaged hunks git-clang-format refuses to apply without `--force`; do NOT pass `--force` (it would stage unstaged work) — detect that case and fail with a message telling the user to stage or stash the rest of the file. Keep the "no modified files to format" / "did not modify any files" exit-0 contract. Bash only, no new dependencies.
  - verify: With clang-format 21.1.8 installed: stage a deliberately misformatted edit to an `Engine/*.cpp`, run `.git-hooks/pre-commit` directly → exit 0, the staged diff is now clang-format-clean (`git clang-format --staged --diff` prints no diff). Stage part of a file and leave the rest unstaged with a violation in it → hook exits 1 with the stage-or-stash message and the index is untouched. `pip uninstall clang-format` → hook exits 1 printing the pip install line. A staged edit under `Documentation/templates/*.c` is left byte-identical.
  - size: M

- [x] M59.P1.T2 — Add an idempotent hook installer
  - files: `tools/install-git-hooks.sh` (new)
  - approach: Small bash script, run from any cwd (`cd "$(git rev-parse --show-toplevel)"`): symlink `.git-hooks/pre-commit` to `.git/hooks/pre-commit` — relative link `../../.git-hooks/pre-commit`, the form README.md already documents. Idempotent: an existing symlink pointing at the right target is a no-op; an existing foreign file is left alone and reported (exit 1) rather than clobbered. Install only `pre-commit` — deliberately not `core.hooksPath=.git-hooks`, because that would also arm `post-commit`, whose `git-version` rewrites the tracked `Global/GitVersion.h` after every commit and dirties the tree (see Decisions). After linking, check the pinned tool (`python3 -c 'import clang_format'` and `git-clang-format` on PATH) and, if missing, print the `pip install --user --break-system-packages clang-format==21.1.8` line — print, don't install; the hook itself is the gate. Also accept `--uninstall` to remove the symlink it created. `shellcheck`-clean, since `lint-ci` runs shellcheck on `*.sh`.
  - verify: `tools/install-git-hooks.sh` twice on this clone → first run creates the link, second run reports already installed, exit 0 both times; `ls -l .git/hooks/pre-commit` shows the relative symlink. Put a stray regular file at `.git/hooks/pre-commit` → script exits 1 without touching it. `--uninstall` removes the link. `shellcheck tools/install-git-hooks.sh` passes.
  - size: S

- [x] M59.P1.T3 — Document the hook and installer
  - files: `README.md` (the `.git-hooks` paragraph around line 125), `tools/ci/local/README.md`
  - approach: In README.md replace the manual `ln -s` recipe with `tools/install-git-hooks.sh`, state that the hook now auto-formats staged C/C++ lines with the pinned `clang-format==21.1.8` (same as CI) and refuses to commit when the tool is missing, and give the pip install line. Drop the sentence that says the hook merely "verifies" style. In `tools/ci/local/README.md` add a short "Git hooks" subsection near the prerequisites pointing at the installer, so a fresh clone that follows that file top-to-bottom ends up with the hook armed. Keep both edits to a paragraph each.
  - verify: `grep -n 'install-git-hooks' README.md tools/ci/local/README.md` hits both files; `grep -n 'ln -s ../../.git-hooks' README.md` hits nothing; `lint-ci`'s markdown/shell checks (whatever `checks.yml` runs) pass locally.
  - size: S

**Verification gate:** On this clone, `tools/install-git-hooks.sh` then a real `git commit` of a deliberately misformatted `Engine/*.cpp` edit produces a commit whose diff is clang-format-clean (`git clang-format --diff HEAD~1` prints no diff); a commit attempted with `clang_format` uninstalled is refused with the install line; the milestone's own PR is green on `format` and `lint-ci`.

## Decisions

- 2026-09-19 — **Auto-format and re-stage, not check-and-block**: user decision. The hook applies `git clang-format --staged` and re-adds the touched files; the goal is that a commit never reaches CI unformatted, not that the developer is told to go fix it.
- 2026-09-19 — **Missing pinned tool fails the commit**: user decision. The existing hook's warn-and-pass is exactly how unformatted commits reach CI today (this host has no clang-format at all). The hook prints the one-line pip install and exits 1.
- 2026-09-19 — **PR #29 review round (Codex, 5 findings, all fixed in `6e8a48ae7`)**: the hook
  accepted any clang-format on PATH (now version-checked against the pin); git-clang-format
  silently skips paths git would quote, and the newline-delimited re-stage parse could not carry
  an embedded newline (now a NUL-delimited preflight fails the commit for either); a failed
  `git add` was masked by the final `echo` (now guarded); the installer's tool check did not
  mirror the hook's module-first/PATH-fallback resolution (now does, with the version check);
  `--uninstall` treated a dangling foreign symlink as "not installed" (now reported, exit 1).
- 2026-09-19 — **Install by symlinking `pre-commit` only, not `core.hooksPath`**: `.git-hooks/post-commit` runs `git-version`, which rewrites the tracked `Global/GitVersion.h` on every commit; arming the whole directory would dirty the working tree after each commit. Retiring `post-commit`/`git-version` is out of scope here.
