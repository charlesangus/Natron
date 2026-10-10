# M88 - Shared Worktree Ccache

Worktree builds currently start cold: ccache's `base_dir` is unset and `hash_dir` is on, so the absolute paths of `.worktrees/<id>/` never match the main checkout's cache entries. The cache is also 95% full at 40 GB. This milestone makes every tree under the main checkout share one cache, and raises its size.

## Phase 88.1: Shared cache

- [x] M88.P1.T1 — Share ccache across the main checkout and its worktrees
  - files: `tools/ci/local/build.sh`, `tools/ci/local/devshell.sh`, `tools/ci/local/README.md`
  - approach: in `build.sh`, before building, export `CCACHE_BASEDIR` as the **main checkout's** root, never the worktree's own root, derived from `git rev-parse --path-format=absolute --git-common-dir` (its parent), so the same value holds in `.worktrees/<id>/` and the main tree. Also export `CCACHE_NOHASHDIR=1`. Set them in `build.sh` rather than as `docker run -e`, so the existing container needs no `--recreate`. Raise devshell's default `CCACHE_MAXSIZE` from 40G to 80G (113 GB free on the host), and make `build.sh` apply the size with `ccache -M` (or `CCACHE_MAXSIZE`), so it takes effect without recreating the container. README: say that worktrees share the cache when they sit at the same depth as the main tree's build dir (`.worktrees/<id>/` does; `build/wt/<id>/` does not), and that debug info in a cache hit can point at the other tree's sources.
  - verify: with the main tree built, a fresh `.worktrees/<id>/` debug build of the same commit gets most compiles from the cache (`ccache -z` before, `ccache -s` after); `ccache -p` inside the build shows `base_dir` set to the main root and `hash_dir = false`; release and debug in the main tree still build.
  - size: M

**Verification gate:** a fresh worktree build of a commit the main tree has built is served mostly from ccache; both build types still build; `format` and `lint-ci` green.

## Decisions
- 2026-10-10 — **Added (user):** worktree builds missed ccache entirely. Runs as its own lane, independent of the other milestones.
- 2026-10-10 — **First measurement is a cold baseline:** existing cache entries were keyed on absolute paths, so the first `base_dir` build hits 0%. The real measurement is a second fresh worktree of the same commit after it.
- 2026-10-10 — **P1.T1** landed (`8b74b054f`). A wiped `.worktrees/m88` debug build took 737/737 compiles from the cache, and a fresh `.worktrees/m88probe` worktree of the same commit took 702/737 (95%); `base_dir` resolves to the main root from a worktree. Release was not built locally: the change only adds environment exports common to every build type, so CI's release build covers it.
