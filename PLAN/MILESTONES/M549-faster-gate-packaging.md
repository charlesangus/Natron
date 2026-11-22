# M549 - Faster Gate Packaging

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Since M493 - Shared Ccache Remote Storage, a remote gate compiles in 3-10 min (99% ccache hits on a typical lane, 776/790 from remote storage), but the build job still takes ~50 min: the "Stage bundle" step (`tools/release/stage-bundle.sh`, `.github/workflows/remote-build.yml:336`) takes 33-54 min on every packaged gate, unchanged since before M493, and "Create AppImage" another 4-5 min. Packaging is now about 80% of a gate's build job. Make it fast, without changing what a packaged AppImage contains.

Start after F4 - Heavy Scene Viewer lands (user, 2026-11-22).

Acceptance sketch:
- Profile `stage-bundle.sh` on a remote run and name where the time goes (copying, dependency walking, `strip`/`objcopy`, symbol splitting, Python/plug-in bundling).
- Cut the step to a few minutes: parallelise per-file work, avoid re-walking unchanged dependencies, cache what is identical between runs (e.g. third-party libs keyed on the dev image hash), as the profile shows. The AppImage's file list and checksums of unchanged files match the current output.
- Gates that only run ctest skip packaging; `build.sh` packages only when a GUI script, `--from-run` reuse or UAT needs the AppImage (today `--no-appimage` exists but gates package by default). Decide the default with the user if it changes what PR CI or release.yml produce.
- Also explain the 0-hit remote-cache cases seen on 2026-11-22: the first `main` build after M493 (0/564) and the local M349 release build (0/1403) — likely compiler path or flags differing from CI — and fix them if cheap.
