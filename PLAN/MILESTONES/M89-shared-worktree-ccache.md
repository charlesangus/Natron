# M89 - Shared Worktree Ccache

Worktree builds currently start cold: ccache's `base_dir` is unset and `hash_dir` is on, so the absolute paths of `.worktrees/<id>/` never match the main checkout's cache entries. The cache is also 95% full at 40 GB. This milestone makes every tree under the main checkout share one cache, and raises its size.

## Phase 89.1: Shared cache

- [x] M89.P1.T1 — Share ccache across the main checkout and its worktrees
  - files: `tools/ci/local/build.sh`, `tools/ci/local/devshell.sh`, `tools/ci/local/README.md`
  - approach: in `build.sh`, before building, export `CCACHE_BASEDIR` as the **main checkout's** root, never the worktree's own root, derived from `git rev-parse --path-format=absolute --git-common-dir` (its parent), so the same value holds in `.worktrees/<id>/` and the main tree. Also export `CCACHE_NOHASHDIR=1`. Set them in `build.sh` rather than as `docker run -e`, so the existing container needs no `--recreate`. Raise devshell's default `CCACHE_MAXSIZE` from 40G to 80G (113 GB free on the host), and make `build.sh` apply the size with `ccache -M` (or `CCACHE_MAXSIZE`), so it takes effect without recreating the container. README: say that worktrees share the cache when they sit at the same depth as the main tree's build dir (`.worktrees/<id>/` does; `build/wt/<id>/` does not), and that debug info in a cache hit can point at the other tree's sources.
  - verify: with the main tree built, a fresh `.worktrees/<id>/` debug build of the same commit gets most compiles from the cache (`ccache -z` before, `ccache -s` after); `ccache -p` inside the build shows `base_dir` set to the main root and `hash_dir = false`; release and debug in the main tree still build.
  - size: M

**Verification gate:** a fresh worktree build of a commit the main tree has built is served mostly from ccache; both build types still build; `format` and `lint-ci` green.

## Decisions
- 2026-10-10 — **Added (user):** worktree builds missed ccache entirely. Runs as its own lane, independent of the other milestones.
- 2026-10-10 — **First measurement is a cold baseline:** existing cache entries were keyed on absolute paths, so the first `base_dir` build hits 0%. The real measurement is a second fresh worktree of the same commit after it.
- 2026-10-10 — **P1.T1** landed (`8b74b054f`). A wiped `.worktrees/m88` debug build took 737/737 compiles from the cache, and a fresh `.worktrees/m88probe` worktree of the same commit took 702/737 (95%); `base_dir` resolves to the main root from a worktree. Release was not built locally: the change only adds environment exports common to every build type, so CI's release build covers it.
- 2026-10-10 — **Renumbered M88 → M89:** the GPU host's PM had added M88 - FIR Gaussian Blur five minutes earlier on its own copy of the plan branch, so this milestone takes the next free ID. The code branch keeps its `milestone/m88-shared-worktree-ccache` name, since renaming it would close PR #45.
- 2026-10-10 — **Review round (Codex, PR #45):** 3 findings, all fixed in `1c4bb4753`: build.sh overrode CI's 6G `CCACHE_MAXSIZE` (now devshell.sh passes the size on every exec and build.sh leaves it alone, which also reaches worktree builds run through devshell); the common-dir lookup now ignores `safe.directory`; the README's depth claim was wrong (sharing depends on the `build/<type>` layout, not worktree depth).
- 2026-10-10 — **CI failure fixed (`225718479`):** under `base_dir`, `__FILE__` is relative to the build dir, so `ShuffleMatrix_Test`'s `__FILE__`-based fixture lookup missed its EXR (5 GuiTests failed twice, deterministic). GuiTests now gets `NATRON_TESTS_FIXTURES_DIR` like Tests; no other code builds paths from `__FILE__`.
- 2026-10-10 — **Shipped:** PR #45 squash-merged to `main` as `feb820bea` after one Codex round (3 findings fixed) and a CI fix; `format`, `lint-ci`, `build-and-test` green.
