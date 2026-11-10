# M394 - Release Versioning

Make the version honest and single-sourced for Nucleon 1.0. CMake's `project(VERSION)` becomes the only version number. The release workflow derives the development status and build number from the tag and passes them to the build. Betas are tagged `v1.0.0-betaN` until 1.0, then `v<next minor>-betaN`. Release candidates are `vX.Y.Z-rcN` and stable releases `vX.Y.Z`. Release titles say Nucleon. A pruning step keeps only the newest ten beta prereleases.

Context:
- **Runs after F17 - OpenNucleon Flag Day** (Deps M386 - Nucleon Branding And Packaging, M387 - Dev Infrastructure Rename), so names are `NUCLEON_*`, `Nucleon` and `NucleonRenderer`.
- **Version today** (pre-rename names):
  - `Global/Macros.h:~123-160` keeps a table of per-release macros (`*_MAJOR_21` … `*_MAJOR_30`) and selects 2.6.0. `CMakeLists.txt:23` also says `VERSION "2.6.0"`.
  - `Macros.h:~198-203` special-cases the 2.2 branch for the docs URL.
  - The development status is picked by `-D…_CONFIG_{SNAPSHOT,ALPHA,BETA,RC,STABLE}` (`Macros.h:178-191`) and falls back to "Devel". No build passes any of them, so every release reports "Devel".
  - The BETA branch sets the status to `…_CONFIG_BETA`, not the string macro (`Macros.h:183`), which is a bug.
  - The build number is hard-wired to 0 (`Engine/CMakeLists.txt:314`).
- **Tags today:**
  - `tools/release/next-beta-tag.sh` hard-codes `v0.1.0-beta<N>`.
  - `.github/workflows/beta-release.yml` tags each green `main` push and dispatches `release.yml`.
  - `release.yml:~203` runs `gh release create "$TAG" --title "Natron $TAG" --generate-notes` (renamed by F17), and marks any tag containing `-` as a prerelease.
  - About 170 `v0.1.0-betaN` releases exist.
- **Update check:** `AppInstance::checkForNewVersion` compares the encoded version, then the status rank, then the build number. Devel and Snapshot builds are never prompted on equal versions (`compareDevStatus`, `AppInstance.cpp:398`). M392 - Docs Publishing writes `LATEST_VERSION.txt` for stable releases only.

## Scope

**In:**
- One version source: `project(VERSION)`, passed to the code as compile definitions. The per-release macro tables and the 2.2 special case are deleted.
- CMake cache variables `NUCLEON_DEV_STATUS` (`Devel` by default, plus `Alpha`, `Beta`, `RC`, `Release`) and `NUCLEON_BUILD_NUMBER` (default 0). This fixes the BETA bug.
- The version set to 1.0.0.
- Tag scheme:
  - betas are `v1.0.0-betaN` before the first stable release and `vX.(Y+1).0-betaN` after `vX.Y.Z`;
  - RCs are `vX.Y.Z-rcN`, dispatched by hand;
  - stable releases are `vX.Y.Z`.
- `release.yml` derives the status and build number from the tag, checks that the tag's `X.Y.Z` equals `project(VERSION)`, titles releases "Nucleon <tag>", and publishes stable releases as non-prerelease.
- Pruning (user, 2026-11-10): keep the newest ten beta prereleases and delete older beta releases and their tags, including the old `v0.1.0-betaN` set.

**Out:**
- Release notes and changelogs: M395 - Release Notes 1.0.
- `LATEST_VERSION.txt` and docs versions: M392 - Docs Publishing.
- Cutting the RC and the 1.0 tag: M396 - Release 1.0 Cut.

**Edge cases:**
- **Tag and version disagree** (for example `v1.1.0-beta1` while CMake says 1.0.0). The release job fails before building and names both values.
- **After 1.0.0, main's version.** The commit that tags `v1.0.0` is followed by a bump of `project(VERSION)` to 1.1.0 on `main`. That step goes in M396 - Release 1.0 Cut's checklist, and `next-beta-tag.sh` then yields `v1.1.0-beta1`. If the bump is forgotten, the edge case above fails the next beta loudly.
- **RCs and stables are never pruned.** Only tags matching `-beta\d+$` are candidates.
- **Concurrent beta tagging** keeps the existing atomic-push lock in `beta-release.yml`.
- **Local and dev builds** keep `Devel` and build number 0, so they never prompt for updates.

**Assumptions:**
- Deleting old beta releases and tags is acceptable for every beta, including all `v0.1.0-betaN` (user, 2026-11-10: rolling and pruned).

## Phase 394.1: Version Source

- [ ] M394.P1.T1 — Single-source the version and the build status in CMake
  - files: `CMakeLists.txt` (`project(VERSION)`, new cache variables), `Global/Macros.h`, `Engine/CMakeLists.txt` (`:~314` build number), any target that consumed the old `*_CONFIG_*` defines
  - approach:
    - Set `project(... VERSION 1.0.0)`.
    - Add the `NUCLEON_DEV_STATUS` cache variable (a STRINGS property listing the allowed values, validated) and the `NUCLEON_BUILD_NUMBER` cache variable.
    - Pass `NUCLEON_VERSION_MAJOR/MINOR/REVISION`, `NUCLEON_DEVELOPMENT_STATUS` (the string) and `NUCLEON_BUILD_NUMBER` as compile definitions on the targets that use them, or through a configured header if that's cleaner.
    - Delete the per-release macro tables, the `*_CONFIG_*` selection and the 2.2 docs special case from `Macros.h`.
  - verify: Configure and build `fast` with `-DNUCLEON_DEV_STATUS=Beta -DNUCLEON_BUILD_NUMBER=7`. `NucleonRenderer --version` reports 1.0.0 Beta 7, and the About dialog shows the same (an Xvfb screenshot goes in the PR body). A default configure reports 1.0.0 Devel. Full ctest passes on the remote `fast` build.
  - size: M

## Phase 394.2: Tags And Workflows

- [ ] M394.P2.T1 — Derive tags, status and build number in the release scripts
  - files: `tools/release/next-beta-tag.sh`, `tools/release/tag-info.sh` (new), `tools/release/tests/` (new), `.github/workflows/{beta-release,release}.yml`
  - approach:
    - `next-beta-tag.sh` finds the newest stable tag `vX.Y.Z` (none means the next version is 1.0.0, otherwise `X.(Y+1).0`) and the highest existing `-betaN` for that version, and prints the next tag.
    - `tag-info.sh <tag>` prints `version=`, `status=` (`Beta`, `RC` or `Release`) and `build=` (N, or 0 for a stable release), and fails on any other shape.
    - `release.yml`:
      - runs `tag-info.sh`;
      - fails if `version` differs from `project(VERSION)`, read with a `cmake -P` one-liner or a grep of `CMakeLists.txt`;
      - passes the status and build number to the build;
      - titles the release "Nucleon $TAG";
      - keeps `--prerelease` for beta and RC only.
    - Add shell tests in `tools/release/tests/`, run by the Lint CI job.
  - verify: The tests cover:
    - `next-beta-tag.sh` with no stable tag giving `v1.0.0-beta1`, then `beta2`;
    - with `v1.0.0` present, giving `v1.1.0-beta1`;
    - `tag-info.sh` on a beta, an RC, a stable and a malformed tag.

    `actionlint` passes. A dispatched release of the next beta on the milestone branch (a draft or prerelease, deleted afterwards) builds with status Beta and the right build number in `--version`.
  - size: M

- [ ] M394.P2.T2 — Prune old beta releases
  - files: `tools/release/prune-betas.sh` (new), `.github/workflows/beta-release.yml` (a step or job after a successful dispatch)
  - approach:
    - `prune-betas.sh [--keep 10] [--dry-run]` lists releases with `gh release list --json tagName,isPrerelease,createdAt`.
    - It keeps the newest ten whose tag matches `-beta[0-9]+$`, and deletes the rest with `gh release delete <tag> --cleanup-tag -y`.
    - It never touches RC or stable tags, or any non-matching tag.
    - The workflow runs it after each beta release.
    - The first run deletes the old `v0.1.0-betaN` set. Run it once by hand with `--dry-run` first and paste the list into the PR body.
  - verify: `--dry-run` against the live repo lists only beta tags beyond the newest ten, and no RC or stable tag. The script's test (in `tools/release/tests/`, with a fake `gh` on `PATH`) covers the mixed beta, RC and stable cases.
  - size: M

**Verification gate:**
- The remote `fast` build and full ctest pass.
- The `tools/release/tests/` suite passes, and `actionlint` passes.
- A dispatched beta release from the branch reports the right version, status and build number, and is titled "Nucleon …".
- The `prune-betas.sh --dry-run` output, in the PR body, lists only betas.

## Decisions

- 2026-11-10 — The first stable release is Nucleon 1.0.0. Betas lead up to it as `v1.0.0-betaN` (user).
- 2026-11-10 — The beta channel stays rolling, one prerelease per green `main` push, pruned to the newest ten (user).
- 2026-11-10 — `project(VERSION)` is the only version source, and the release workflow derives the status and build number from the tag: the old macro tables and the never-passed `*_CONFIG_*` flags made every release report "Devel" (planner).
