# M549 - Faster Gate Packaging

Since M493 - Shared Ccache Remote Storage, a remote gate compiles in 3-10 min (99% ccache hits on a typical lane, 776/790 from remote storage), but the build job still takes ~50 min: the "Stage bundle" step (`tools/release/stage-bundle.sh`, `.github/workflows/remote-build.yml:336`) takes 33-54 min on every packaged gate, unchanged since before M493, and "Create AppImage" another 4-5 min. Packaging is now about 80% of a gate's build job. Make it fast, without changing what a packaged AppImage contains.

Start after F4 - Heavy Scene Viewer lands (user, 2026-11-22).

Acceptance sketch:
- Profile `stage-bundle.sh` on a remote run and name where the time goes (copying, dependency walking, `strip`/`objcopy`, symbol splitting, Python/plug-in bundling).
- Cut the step to a few minutes: parallelise per-file work, avoid re-walking unchanged dependencies, cache what is identical between runs (e.g. third-party libs keyed on the dev image hash), as the profile shows. The AppImage's file list and checksums of unchanged files match the current output.
- Gates that only run ctest skip packaging; `build.sh` packages only when a GUI script, `--from-run` reuse or UAT needs the AppImage (today `--no-appimage` exists but gates package by default). Decide the default with the user if it changes what PR CI or release.yml produce.
- Also explain the 0-hit remote-cache cases seen on 2026-11-22: the first `main` build after M493 (0/564) and the local M349 release build (0/1403) — likely compiler path or flags differing from CI — and fix them if cheap.

## Scope

Profile of F4's release gate (job 137225766600): Stage bundle 58m18, of which the ldd closure walk (`walk_closure`, `tools/release/stage-bundle.sh:512-597`) is ~52 min — 5m25 copying 240 libs, then ~47 min silently re-walking already-staged files (an `ldd` per binary, an `echo | sed` and `basename` fork per output line, a 70-pattern excludelist loop per line, push-before-seen-check and an O(n²) `queue=("${queue[@]:1}")` pop). `check-relocatable.sh` (892 binaries, an `objdump` per NEEDED entry) runs twice, ~5 min each (stage-bundle and `make-appimage.sh:64`). Strip, USD, MoonRay, Python and plugin staging total under 1 min; there is no symbol splitting. appimagetool takes 22 s. Packaging also delays ctest by ~64 min, since the ctest job waits on the build job's outputs (`remote-build.yml:408-411`).

Out of scope: caching staged libs per image (not needed once the walk is fixed); changing what `ci.yml` or `release.yml` produce.

## Phase 549.1: Measure

- [x] M549.P1.T1 — Section timings and a staged-file manifest
  - files: `tools/release/stage-bundle.sh`, `tools/release/stage-manifest.sh` (new), `.github/workflows/remote-build.yml`
  - approach: log `$SECONDS` per `==>` section and for the walk; `stage-manifest.sh <dir>` writes path, mode, symlink target and sha256 per staged file, sorted. The remote build uploads the manifest with the AppImage.
  - verify: one remote release run with the current walk records the baseline manifest and section times.
  - size: S

## Phase 549.2: Fix the scripts

- [ ] M549.P2.T2 — Fork-free, linear closure walk
  - files: `tools/release/stage-bundle.sh`
  - approach: parse `ldd` output with `[[ =~ ]]`, `${dep##*/}`, one extglob for the excludelist; an index-based queue; check SEEN before pushing. `ldd` a breadth-first frontier in parallel (`xargs -P`) but apply copy decisions serially in the same order, so "first source for a basename wins" is unchanged.
  - verify: on the same build tree the manifest matches the T1 baseline byte for byte; the walk takes under 2 min.
  - size: M
- [ ] M549.P2.T3 — One relocatability check, run in parallel
  - files: `tools/release/check-relocatable.sh`, `tools/release/make-appimage.sh`, `tools/release/stage-bundle.sh`
  - approach: one `objdump -p` per binary, binaries checked in parallel; stage-bundle leaves a stamp with the manifest hash and `make-appimage.sh` skips the re-check when the stamp matches the staged tree. The checker keeps its own resolution code (independent of the stager).
  - verify: a scratch stage with a broken RUNPATH still fails; the check takes under 1 min; Create AppImage under 1 min.
  - size: M

## Phase 549.3: Package only when needed

- [ ] M549.P3.T4 — Gates package on demand 
  - files: `tools/ci/remote/build.sh`, `.github/workflows/remote-build.yml`, `tools/ci/local/README.md`
  - approach: gates default to no AppImage; `--gui`, a new `--appimage` flag (UAT) or `--package` for a run meant as a `--from-run` source turn it on; `--from-run` errors clearly when its source has no AppImage.
  - verify: dispatches with and without `--gui` show the expected job lists.
  - size: M
- [ ] M549.P3.T5 — Package in parallel with ctest
  - files: `.github/workflows/remote-build.yml`
  - approach: move stage, AppImage and upload into a `package` job fed by the build-tree artifact; ctest no longer waits for packaging; the gui job needs `package`.
  - verify: ctest starts before packaging ends; the manifest is identical to T1's baseline.
  - size: M
- [ ] M549.P3.T6 — Ccache note
  - files: `tools/ci/local/README.md` (or the ccache section it has)
  - approach: the 2026-11-22 0-hit builds were lanes without M493 in their history (their gates never wrote to remote storage) and rarely built `release` objects; no config mismatch. Say so in one paragraph.
  - verify: read.
  - size: S

**Verification gate:** a remote release gate with `--gui` is green; its manifest matches the baseline; Stage bundle under 5 min and Create AppImage under 1 min.

## Decisions

- 2026-11-22 — Profile done (consultant, log /tmp/m549log on the dev host): the cost is the walk and the doubled checker, not strip or copying; no per-image lib cache. The 0-hit remote-cache builds were lanes predating M493, not a config mismatch.
- 2026-11-22 — T1 in 18b55e6c5 (artifact stage-manifest-<preset>-<sha>); baseline `fast` run queued; Phase 2 being implemented.
- 2026-11-22 — **Gates package on demand (user):** jobs that don't need an AppImage stop building one; `--gui`, UAT builds and `--from-run` sources still package. PR CI and release.yml unchanged.
