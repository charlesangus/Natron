# M385 - OpenNucleon Code Rename

The flag day for the code. Run `tools/rename/rename.py` over the whole tree, make the hand fixes M383 - Rename Script And Guard's `DRYRUN.md` lists, merge and pin the plug-in fork rename branches from M384 - Plugin Forks Rename, and switch on the guard. Afterwards the app is Nucleon throughout: the C++ namespace, `NUCLEON_*` macros, binaries, Python modules, settings and cache paths, environment variables, file extensions, MIME types, plug-in IDs and the OFX host name. The mapping is `DECISIONS/2026-11-10-rename-to-opennucleon.md`. Publish that decision to `docs/decisions/` at the gate. It supersedes `docs/decisions/2026-08-29-keep-natron-branding.md`.

**Held:** starts only at the user's go-ahead, as part of F17 - OpenNucleon Flag Day (see `# Open questions`).

Context:
- Read `tools/rename/DRYRUN.md` first. It lists the per-rule counts, the path renames and the hits the rules leave for hand fixing.
- **Settings and cache paths** come from Qt's organisation and application names (`Engine/AppManager.cpp`, the `setOrganization*` and `setApplicationName` calls) and from the hardcoded `~/.cache/Natron/gpu-pipelines` in `Engine/Gpu/GpuPipelineCache.cpp:67,71`. The fork keeps no backward compatibility, so no old settings are migrated.
- **Python:** `Engine/typesystem_engine.xml` (`package="NatronEngine"`, `<namespace-type name="Natron">`), `Gui/typesystem_natronGui.xml`, and the global `natron` object (`Engine/AppManager.cpp`, `AppManagerPrivate.cpp`, `AppInstance.cpp`). Strings such as `NatronEngine.Natron.KeyframeTypeEnum.*` are baked in at `Engine/NodeGroup.cpp:1734-1758`.

## Scope

**In:**
- The whole-tree rename as one mechanical commit, then the hand fixes as separate commits.
- Merging the fork rename branches, pinning them, and moving the `libs/OpenFX` submodule URL to `opennucleon/openfx-nucleon`.
- The `--check` guard in `checks.yml` (the Code rules job) and `.git-hooks/pre-commit`.
- A migration note for open branches in the PR body and in `tools/rename/README.md`.

**Out:**
- Art, the splash code, the About dialog, desktop and AppStream files, AppImage names and the README: M386 - Nucleon Branding And Packaging. The script renames identifiers in those files, but their content and art change there.
- The dev image, container names and workflow image references: M387 - Dev Infrastructure Rename.
- Licence text and headers: M157 - Relicense To GPLv3 Or Later, which lands first in the Feature.
- Rewriting plan milestone briefs: a `/cat-plan` sweep after the Feature lands (see `# Open questions`).

**Edge cases:**
- **Open branches and Features.** After F17 - OpenNucleon Flag Day lands, every open branch merges `main`, then runs `rename.py --changed-since origin/main` and commits the result. The guard makes a branch that skips this fail CI. Say so in the PR body.
- **Tests with hardcoded names.** Tests expecting `NatronEngine`, `fr.inria.*`, `.ntpz` fixture names or the `natron` Python global are handled by the script. Any failure left after the script runs is a hand fix, never a guard allowlist entry.
- **LFS fixtures named `*.ntpz`** are renamed with `git mv`. This changes no LFS objects.
- **The `NATRON_GITVERSION_H_` generator** in `.git-hooks/git-version` is renamed with everything else.

**Assumptions:**
- The user has transferred the repo and the forks to the `opennucleon` org before this starts (the F17 - OpenNucleon Flag Day open question).
- M383 - Rename Script And Guard's dry run is still representative. Re-run it first; new hits become rules.

## Phase 385.1: Whole-Tree Rename

- [ ] M385.P1.T1 — Refresh the dry run against the current Feature branch
  - files: `tools/rename/rules.toml`, `tools/rename/allowlist.txt`, `tools/rename/DRYRUN.md`
  - approach: Merge `main` into the Feature branch first (§9b). Repeat M383.P2.T2's dry run on a copy, and add rules or allowlist lines for anything new since it was written.
  - verify: `--check` on the renamed copy reports only the hand fixes listed in `DRYRUN.md`.
  - size: M

- [ ] M385.P1.T2 — Run the rename over the tree as one mechanical commit
  - files: the whole first-party tree (mechanical, via `python3 tools/rename/rename.py`)
  - approach: Run the script on the milestone branch. Commit its output on its own, with no hand edits, so review can trust the rule table instead of the diff. Run `git clang-format` against the merge base, and commit any reformatting caused by the longer names as a second commit.
  - verify: `rename.py --check` reports only `DRYRUN.md`'s hand-fix list. A second `rename.py` run is a no-op. `tools/ci/local/syntax-check.py` on 30 sample TUs from Engine, Gui, Serialization and Tests is clean (moc TUs excluded; see the syntax-check caveat).
  - size: M

- [ ] M385.P1.T3 — Apply the hand fixes and get the build green
  - files: the files `DRYRUN.md` lists, plus whatever the build reports
  - approach: Apply each hand fix in `DRYRUN.md`, then run one remote `fast` build with `--tests`. Fix the fallout, which is mostly Shiboken-generated names, CMake target references and test expectations. Any fix that a rule could have made goes back into `rules.toml`, so open branches get it too.
  - verify: The remote `fast` build and full ctest pass, and `rename.py --check` exits 0.
  - size: L

## Phase 385.2: Plug-in Forks

- [ ] M385.P2.T1 — Rebase, merge and pin the fork rename branches
  - files: `tools/rename/forks.toml`, `tools/ci/local/fetch-assets.sh`, `.gitmodules`, `libs/OpenFX` (submodule pin)
  - approach: For each fork in `forks.toml`:
    1. Rebase its `opennucleon-rename` branch onto the fork's current default branch.
    2. Re-run `rename.py --root`.
    3. Rebuild it.
    4. Merge it into the default branch through a PR on the fork.
    5. Pin the merge SHA in `fetch-assets.sh` (`OPENFX_*_REF`, including the repository URLs, now under `opennucleon/`) and in the `libs/OpenFX` submodule, whose URL becomes `https://github.com/opennucleon/openfx-nucleon.git`.

    Then run `git submodule sync`. Update `forks.toml` to the merged SHAs.
  - verify: `fetch-assets.sh` from clean, then the remote `fast` build with full ctest, passes. ctest's plug-in sweep loads every bundled plug-in under its `io.github.opennucleon.*` ID.
  - size: L

- [ ] M385.P2.T2 — Prove the plug-ins see the Nucleon host
  - files: `Tests/` (one new test file, e.g. `Tests/OfxHostIdentity_Test.cpp`, plus its registration in the Tests CMake list)
  - approach: A gtest loads an openfx-misc plug-in that uses a Nucleon-only extension (the channel selector, per M384.P1.T1's survey). It asserts that the host reports `io.github.opennucleon.Nucleon` and that the plug-in describes the extension, which it only does when `isNucleon` is true.
  - verify: The test passes against the pinned bundles. It fails if the host name is changed back to `fr.inria.Natron`; check that locally once and note the result in `## Decisions`.
  - size: M

## Phase 385.3: Guard And Hand-Off

- [ ] M385.P3.T1 — Wire the guard into CI and the pre-commit hook
  - files: `.github/workflows/checks.yml`, `.git-hooks/pre-commit`, `tools/rename/README.md` (new)
  - approach: Add a step running `python3 tools/rename/rename.py --check` to the Code rules job in `checks.yml`, following how its existing check steps run. In the pre-commit hook, run `--check --staged`. The README explains the rule table, the allowlist, and the open-branch recipe: merge main, run `rename.py --changed-since origin/main`, then commit.
  - verify: On a scratch branch, a new line containing `Natron` fails both the hook and the CI step, and the recipe fixes it. Dispatch `checks.yml` on the milestone branch and confirm it is green.
  - size: M

**Verification gate:**
- The remote `fast` build and full ctest pass.
- `rename.py --check` exits 0.
- `git grep -il natron -- ':!docs/decisions' ':!libs'` lists only allowlisted lines.
- In the AppImage under Xvfb:
  - the binary is `Nucleon`
  - the Python console imports `NucleonEngine`, and the `nucleon` global exists
  - settings are written to `~/.config/OpenNucleon/Nucleon.conf`
  - Save writes a `.nucz` file
  - the GPU pipeline cache lands under `~/.cache/Nucleon/`
- `DECISIONS/2026-11-10-rename-to-opennucleon.md` is published to `docs/decisions/`.
- The PR targets `feature/f17-opennucleon-flag-day`.
