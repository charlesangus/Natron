# M383 - Rename Script And Guard

Write the tool that performs the Natron → Nucleon rename, plus the guard that keeps the name from coming back. `tools/rename/rename.py` rewrites file contents and renames paths from one ordered rule table. It is idempotent, so it can run first over the whole tree at the flag day and later over each open branch's own diff. `--check` is the guard. This milestone only builds and tests the tool on fixtures and a dry run. M385 - OpenNucleon Code Rename runs it for real and wires the guard into CI.

The full set of names is in `DECISIONS/2026-11-10-rename-to-opennucleon.md`. The rule table must implement exactly that file's mapping.

Context:
- **Scale on `main` (2026-11-10):** about 2,600 files mention `Natron` and about 2,600 mention `NATRON_`, with 809 distinct `NATRON_*` macros. The namespace is `namespace Natron` with `NATRON_NAMESPACE` (`Global/Macros.h:45-75`). About 207 lines use `Natron::` explicitly.
- **Names built from macros.** The organisation macros in `Global/Macros.h:79-86` build the OFX host name (`Engine/Settings.cpp`, the `pluginHostName` choice) and the QCoreApplication organisation and domain (`Engine/AppManager.cpp`). Rewriting the macro values covers those.
- **`fr.inria` leftovers on `main`:**
  - the seven bundled PyPlugs: `Gui/Resources/PyPlugs/{ZRemap,ZMask,SplitAndJoin,PIKColor,LightWrap,DropShadow,AngleBlur}.py`
  - `PLUGINID_OFX_*` in `Engine/EffectInstance.h:58-62`
  - the openfx-io IDs in `Engine/OfxEffectInstance.cpp` and `Engine/Plugin.cpp`
  - `Gui/ExportGroupTemplateDialog.cpp:173`
- **Paths carrying the name** include:
  - `App/NatronApp_main.cpp`, `Renderer/NatronRenderer_main.cpp`
  - `Gui/typesystem_natronGui.xml`
  - `cmake/Natron*.cmake`
  - `Gui/Resources/Icons/natron/`
  - `Gui/Resources/TabTabTab/tabtabtab_natron.py`
  - `Tests/OfxTestPlugins/natronTest/`
  - `skills/natron-maintainer/`
  - `Engine/Nodes/Scene/shaders/natronProject3D.glslfx`
  - the `fr.natron.Natron.*` desktop and appdata files
  - every `*Natron*.h` and `*Natron*.cpp`

## Scope

**In:**
- A rule-table rename engine for contents and paths, with an allowlist, a `--changed-since <ref>` mode for open branches and a `--root <dir>` mode for the plug-in forks.
- `--check` guard mode and its allowlist.
- Unit tests on fixtures, and a dry-run report against current `main` that lists every hit no rule covers.

**Out:**
- Running the rename on the real tree, and wiring `--check` into `checks.yml` and the pre-commit hook: M385 - OpenNucleon Code Rename.
- Artwork files: M382 - Nucleon Logo And Artwork and M386 - Nucleon Branding And Packaging.
- The plug-in forks themselves: M384 - Plugin Forks Rename (which uses `--root`).
- Rewriting `docs/decisions/*.md` history (user, 2026-11-10).

**Edge cases:**
- **Preserved credit.** The allowlist keeps `(C) … The Natron developers` and INRIA copyright lines, the "a fork of Natron" credit line, `docs/decisions/` and vendored `libs/` other than `libs/OpenFX_extensions/`. They are allowlisted by line pattern, not by whole file, so the rest of each file is still renamed.
- **Case variants.** `NATRON` → `NUCLEON`, `Natron` → `Nucleon` and `natron` → `nucleon`. Apply longer and more specific rules first: URLs, reverse-DNS IDs, extensions and the org names come before the bare word.
- **Extensions.** Rewrite `ntpz` → `nucz`, `ntc` → `nucc`, `nl` → `nucl` and `nps` → `nucp` only in macro definitions, globs, MIME XML, test fixture names and string literals ending in that extension. Never touch a bare `nl` token.
- **Idempotence.** A second run is a no-op. No rule's output may match another rule's input, for example `io.github.opennucleon` must never be re-prefixed.
- **Binary files** are skipped for content rewriting but still path-renamed. Git LFS pointer files are left alone.
- **Path collisions.** If a path rename would overwrite an existing file, the tool aborts with both paths.

**Assumptions:**
- Unconfirmed: the dry run reveals few enough uncovered hits that they can become rules or allowlist lines here, rather than hand edits at the flag day.

## Phase 383.1: Rename Engine

- [ ] M383.P1.T1 — Write the rule table and the content rewriter
  - files: `tools/rename/rename.py` (new), `tools/rename/rules.toml` (new), `tools/rename/allowlist.txt` (new)
  - approach:
    - `rules.toml` holds ordered literal or regex rules, each with optional path globs it is limited to. Seed it from the mapping in `DECISIONS/2026-11-10-rename-to-opennucleon.md`.
    - `allowlist.txt` holds path globs to skip entirely and line regexes to leave untouched.
    - `rename.py [--root DIR] [--dry-run]` walks `git ls-files` under the root, skips binary and LFS files, and applies the rules line by line, except to allowlisted lines.
    - `--dry-run` prints a per-rule count and the changed files without writing anything.
    - Standard library only, apart from `tomllib`, which is stdlib in Python 3.11+.
  - verify: P1.T3's unit tests.
  - size: M

- [ ] M383.P1.T2 — Add path renames and the changed-since mode
  - files: `tools/rename/rename.py`, `tools/rename/rules.toml`
  - approach:
    - Add a `[paths]` rule set and apply it to file and directory names with `git mv`, deepest paths first, aborting on a collision.
    - Path renames run after content rewrites, so includes and CMake source lists that name a renamed file are already rewritten. The content rules must therefore cover names such as `NatronApp_main.cpp`.
    - `--changed-since <ref>` restricts both passes to the files `git diff --name-only <ref>...HEAD` lists, plus any file those include by a renamed name.
  - verify: P1.T3's unit tests cover a renamed directory, a renamed header included from another file, a collision, and `--changed-since` touching only the diffed files.
  - size: M

- [ ] M383.P1.T3 — Write unit tests on a fixture tree
  - files: `tools/rename/test_rename.py` (new), `tools/rename/fixtures/` (new, a small fake tree)
  - approach: The fixture tree holds a header with a `NATRON_` macro, the namespace macro and an include guard; a `Natron*.cpp` that an include line references; a `CMakeLists.txt` listing it; a PyPlug with `fr.inria.X`; a licence header with the preserved copyright lines; a line with a bare `nl` token; a binary file; and a `docs/decisions/` file. Copy the fixture into a temp git repo for each test. Cover: the expected output after one run, idempotence on a second run, the allowlist, the extension rules leaving `nl` alone, `--changed-since`, and `--dry-run` writing nothing.
  - verify: `python3 -m pytest tools/rename/` passes inside natron-dev.
  - size: M

## Phase 383.2: Guard And Dry Run

- [ ] M383.P2.T1 — Add the --check guard mode
  - files: `tools/rename/rename.py`, `tools/rename/test_rename.py`
  - approach: `--check [paths…]` scans the given files, or all tracked files, for a case-insensitive `natron` or a `fr.inria` outside the allowlist. It prints `file:line` for each hit and exits 1, and its message tells the user to run `python3 tools/rename/rename.py --changed-since origin/main`. With `--staged`, it checks only the staged files, for the pre-commit hook. Do not wire it into CI or the hook here.
  - verify: Tests show `--check` failing on the unrenamed fixture, passing after `rename.py`, and failing again when a new `Natron` line is added.
  - size: M

- [ ] M383.P2.T2 — Dry-run against main and close the gaps
  - files: `tools/rename/rules.toml`, `tools/rename/allowlist.txt`, `tools/rename/DRYRUN.md` (new)
  - approach:
    1. Copy the current feature-branch tree (which tracks `main`) to a temp directory.
    2. Run `rename.py`, then `--check`, on the copy.
    3. For every hit `--check` still reports, add a rule or an allowlist line, or list it in `DRYRUN.md` as a hand fix for M385 - OpenNucleon Code Rename, with the reason.
    4. Also record per-rule counts and the full list of path renames.
    5. Run `tools/ci/local/syntax-check.py --root <copy>` on a sample of 20 renamed TUs and record the result.
  - verify: `--check` on the renamed copy reports only the hits `DRYRUN.md` lists as hand fixes. The syntax-check sample is clean, or each failure is listed in `DRYRUN.md` with its cause.
  - size: L

**Verification gate:**
- `python3 -m pytest tools/rename/` passes.
- `DRYRUN.md` is committed, and its list of leftover hits is short enough to review by hand.
- No file outside `tools/rename/` changed: the real tree is not renamed in this milestone.
- The PR targets `feature/f17-opennucleon-flag-day`.
