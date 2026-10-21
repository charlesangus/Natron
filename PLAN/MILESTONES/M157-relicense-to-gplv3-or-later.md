# M157 - Relicense To GPLv3 Or Later

Move the fork's first-party code from GPL-2.0-or-later to GPL-3.0-or-later, so Apache-2.0 code can be integrated directly. Every existing header already grants "or any later version", so the change itself is a licence-text and header rewrite. The one dependency problem the audit found gets removed first: six openfx-arena plug-ins whose files are GPLv2-only.

Background, the audit results and the user's choices are in `DECISIONS/2026-10-21-relicense-gplv3-or-later.md`. Publish it to `docs/decisions/` at the gate.

Context:
- **Header shape.** The standard Natron block lives in about 1133 first-party files: `.cpp`, `.h`, the 16 `CMakeLists.txt`, the two typesystem `.xml` files, `tools/ci/smoke_test.py` and `libs/OpenFX_extensions/`. The templates are in `tools/license/gpl-header-{c,hash,xml,bat}.txt`. About 156 first-party files have no header at all; most were added by the fork (`Engine/Gpu/**`, `Tests/Gpu/**`, `Tests/gui/**`, `tools/bench/**`, `tools/gpu-spike/**`, the `pch.h` files, `Global/GitVersion.h`).
- **Third-party files inside first-party directories keep their own licence and must never be rewritten:**
  - `Engine/Noise.{h,cpp}` and `Engine/NoiseTables.h`: SeExpr, Apache-2.0.
  - `Gui/QtColorTriangle.{h,cpp}`: Qt Solutions.
  - `Gui/FileTypeMainWindow_win.{h,cpp}`: BSD.
  - `Global/glad*` and `Global/glad_source.c`: MIT.
  - `Global/FloatingPointExceptions.h`: cctbx, licence not yet stated.
  - `Gui/Resources/TabTabTab/`, the fonts and the Lucide icons.
- **The About dialog** shows `LICENSE.txt` (`Gui/GuiResources.qrc`, `Gui/AboutWindow.cpp:459`), `LICENSE_SHORT.txt` (`Gui/AboutWindow.cpp:129`) and the third-party list in `tools/license/components/`.
- **The arena fork** is `charlesangus/openfx-arena`, pinned as `OPENFX_ARENA_REF` in `tools/ci/local/fetch-assets.sh`. Fork changes follow the standing fork-and-fix pattern described there: open a PR on the fork, merge it, then pin the new exact SHA.

## Scope

**In:**
- Dropping the six GPLv2-only arena plug-ins from the shipped Arena bundle.
- Natron's `LICENSE.txt`, `LICENSE_SHORT.txt`, README, appdata, the component list and CONTRIBUTING.
- An SPDX header on every first-party source file.
- A header checker that also runs in CI.

**Out:**
- Relicensing openfx-io, openfx-misc, the rest of openfx-arena or `libs/SequenceParsing`. They stay GPL-2.0-or-later (user decision, 2026-10-21).
- Rewriting the dropped plug-ins, natively or clean-room. A replacement would be a later native-node milestone in E3 - Native Nodes And I/O.
- Updating the copyright years in existing headers.
- Deleting Windows-only third-party files such as `Gui/FileTypeMainWindow_win.*`.
- Any vendored library under `libs/`, apart from `libs/OpenFX_extensions/`, which is Natron's own code.

**Edge cases:**
- **Open branches.** Other open branches add files with the old v2 header. Once the CI guard lands they fail it, and each rebases with `tools/license/headers.py --fix`. Say this in the PR body.
- **Generated PyPlugs.** `Gui/Resources/PyPlugs/*.py` files are generated exports. The checker skips them; it does not stamp them.
- **Overlap with other milestones.** The header rewrite touches nearly every source file, so the claim's file-overlap test will make this milestone wait for every `doing` milestone. That is expected. The rewrite only changes the comment block at the top of each file, so other branches rebase over it with trivial conflicts at most.

**Assumptions:**
- The only GPLv2-only files on the shipped path are `Magick/Sketch.cpp` and `Magick/MagickPlugin.h`. The audit established this by reading headers at the pinned SHAs, not by legal review.
- An SPDX identifier plus a pointer to `LICENSE.txt` is a sufficient grant notice.

## Phase 157.1: Arena GPLv2-Only Plug-ins Out

- [ ] M157.P1.T1 — Drop the GPLv2-only Magick plug-ins from the openfx-arena fork build
  - files: `charlesangus/openfx-arena`: `CMakeLists.txt` (the Arena.ofx source list, which has `"Magick/Sketch.cpp"` at about line 147), and `Magick/{Sketch.cpp, MagickPlugin.h, MagickPlugin.cpp, Swirl/, Morphology/, Roll/, HaldCLUT/, Wave/}`
  - approach: Remove `Sketch`, `Swirl`, `Morphology`, `Roll`, `HaldCLUT` and `Wave` from the source list, along with the `MagickPlugin.{h,cpp}` base they share. Delete those files from the fork too, so nothing GPLv2-only stays in the tree. Leave the OCL directory alone; it is already unbuilt. Open a PR on the fork, merge it, and record the merge SHA in this milestone's `## Decisions`.
  - verify: Build the fork the way `fetch-assets.sh` does. `strings Arena.ofx | grep -E 'net\.fxarena\.openfx\.(Sketch|Swirl|Morphology|Roll|HaldCLUT|Wave)'` prints nothing, and `grep -rL 'later' --include='*.cpp' --include='*.h' Magick Extra Text` finds no GPL file without "or later".
  - size: M

- [ ] M157.P1.T2 — Pin the new arena ref and remove host references to the six plug-ins
  - files: `tools/ci/local/fetch-assets.sh`, `Engine/OfxEffectInstance.cpp` (about lines 1346-1390), `Engine/Plugin.cpp:101`, `Tests/DefaultChannelSet_Test.cpp`, `Tests/ColorViewsRender_Test.cpp:1506`
  - approach: Set `OPENFX_ARENA_REF` to T1's merge SHA, and note the dropped plug-ins and the reason in its comment block. Delete the six `net.fxarena.openfx.*` IDs from the host's per-plugin lists and from the test expectation tables. Run `grep -rn 'fxarena.openfx.\(Sketch\|Swirl\|Morphology\|Roll\|HaldCLUT\|Wave\)'` across `Engine/ Gui/ Tests/ tools/` and clear any further hits, including the M143 - Bundled Plugin Bug Fixes sweep lists.
  - verify: The grep returns nothing. A fresh `fetch-assets.sh` run, followed by ctest's plug-in sweep and `DefaultChannelSet`, passes against the new bundle.
  - size: M

## Phase 157.2: Licence Texts And Docs

- [ ] M157.P2.T1 — Replace the licence texts with GPLv3
  - files: `LICENSE.txt`, `LICENSE_SHORT.txt`, `tools/license/components/LICENSE-GPL-3.0.txt` (new)
  - approach: Make `LICENSE.txt` the verbatim GPLv3 text, copied byte for byte from `/usr/share/common-licenses/GPL-3` or https://www.gnu.org/licenses/gpl-3.0.txt, and copy it to `tools/license/components/LICENSE-GPL-3.0.txt`. Keep `LICENSE-GPL-2.0.txt`, because the forks remain v2+. Change `LICENSE_SHORT.txt` to say "version 3 of the License, or (at your option) any later version", point it at `https://www.gnu.org/licenses/`, and drop the FSF postal address.
  - verify: `sha256sum` of `LICENSE.txt` matches the gnu.org gpl-3.0.txt; `grep -c 'version 2' LICENSE_SHORT.txt` prints 0.
  - size: S

- [ ] M157.P2.T2 — Update the README, the appdata and CONTRIBUTING
  - files: `README.md`, `Gui/Resources/Metainfo/fr.natron.Natron.appdata.xml`, `CONTRIBUTING.md`
  - approach:
    - README: change the badge (line 3) to `license-gpl3-blue` and the "GPLv2 license" text (line 7) to GPLv3-or-later.
    - appdata: set `<project_license>` to `GPL-3.0-or-later` (line 8) and fix the "(GPLv2 license)" prose (line 11).
    - CONTRIBUTING: add a short paragraph near the "same terms" clause (lines 57-60). It says that contributions are GPL-3.0-or-later and new files carry the SPDX header. It also says third-party code under a GPLv3-compatible licence (Apache-2.0, MIT, BSD, MPL-2.0, LGPL) may be vendored if it keeps its own licence text and is listed in `tools/license/components/`.
  - verify: `grep -rn 'gpl2\|GPLv2\|GPL-2.0' README.md Gui/Resources/Metainfo/` finds nothing. `appstreamcli validate` passes on the appdata if it is available in natron-dev; otherwise `xmllint --noout` does.
  - size: S

- [ ] M157.P2.T3 — Rewrite the stale third-party component list
  - files: `tools/license/components/README.md`, plus a `LICENSE-*.txt` beside it for each component it adds
  - approach: The current list names Qt 4.8, FFmpeg, x264, x265 and PySide 1, none of which ship. Rewrite it from what the build actually links and bundles: `CMakeLists.txt`, `cmake/Fetch*.cmake`, `tools/ci/local/fetch-assets.sh`, `tools/ci/local/Dockerfile` (the ASWF base image) and the AppImage contents from `package.sh`.
    - Give each component and its licence, and add a missing licence text file where the component needs one.
    - Mark the OFX plug-in bundles as GPL-2.0-or-later, and note that the six arena plug-ins were dropped.
    - Look up the cctbx licence for `Global/FloatingPointExceptions.h` and record it here.
    - Check how `Gui/AboutWindow.cpp` loads this directory, so any new file actually shows up in the About dialog.
  - verify: Every library in `ldd` of the packaged `Natron` binary and the bundled `.ofx` files that is not a glibc or system library maps to an entry in the list. The About dialog's third-party tab shows the new list (checked in the gate).
  - size: M

## Phase 157.3: SPDX Headers And CI Guard

- [ ] M157.P3.T1 — Write a header checker and fixer, and new templates
  - files: `tools/license/headers.py` (new), `tools/license/gpl-header-{c,hash,xml,bat}.txt`, `tools/license/third-party-paths.txt` (new)
  - approach:
    - New templates: an `SPDX-License-Identifier: GPL-3.0-or-later` line, the "This file is part of Natron <https://natrongithub.github.io/>," line, the two existing copyright lines unchanged, and "See LICENSE.txt for the licence terms." Write them in C, hash and XML comment syntax.
    - `headers.py --check [paths]` walks first-party sources: `.cpp .h .c .py .xml CMakeLists.txt` under `App Engine Gui Global Renderer PythonBin Serialization Tests tools libs/OpenFX_extensions`. It skips everything listed in `third-party-paths.txt` (seeded from this milestone's Context), `Gui/Resources/PyPlugs/`, `Tests/google-*` and every vendored lib. It exits 1 on any file that lacks the SPDX line or still contains "version 2 of the License".
    - `--fix` replaces an existing `BEGIN LICENSE BLOCK … END LICENSE BLOCK` with the template, preserving any additional copyright lines the old block carried. It inserts the template into a headerless file after any shebang or XML declaration line.
  - verify: Copy the tree to a temp dir and run `--fix` there. `--check` on the result exits 0, and on the original tree it exits 1. `git diff --no-index` on three sample files of different types shows only the header changed. Every third-party path is untouched.
  - size: M

- [ ] M157.P3.T2 — Rewrite every first-party header and add attribution to unlabelled third-party files
  - files: about 1300 first-party sources (mechanical, via `tools/license/headers.py --fix`), `Global/FloatingPointExceptions.h`, `Global/glad_source.c`
  - approach: Run `--fix` over the whole first-party set. Separately, give `Global/FloatingPointExceptions.h` an attribution header with its own licence (cctbx, as found in M157.P2.T3), and give the glad files their MIT/glad SPDX line. Do not give either a GPL header. Commit the mechanical rewrite as its own commit, separate from the hand edits, so review can skim it.
  - verify: `headers.py --check` exits 0. `git diff -U0 | grep '^[-+]' | grep -vE '^(\+\+\+|---)'` contains only comment lines. `build/syntax-check.py` on a sample of the changed files is clean. `git clang-format` against the merge base reports nothing.
  - size: M

- [ ] M157.P3.T3 — Run the header check in CI and the pre-commit hook
  - files: `.github/workflows/checks.yml`, `.git-hooks/pre-commit`
  - approach: Add a step to `checks.yml` that runs `python3 tools/license/headers.py --check`, mirroring how the existing format check job is set up. In `.git-hooks/pre-commit`, run `--check` on the staged first-party files only. Its failure message names `--fix`.
  - verify: On a scratch branch, adding a file with the old v2 header makes the hook and the CI step fail, and `--fix` makes both pass. Dispatch `checks.yml` manually on the milestone branch and confirm it is green.
  - size: S

**Verification gate:**
- `tools/license/headers.py --check` exits 0.
- `grep -rln 'version 2 of the License' App Engine Gui Global Renderer PythonBin Serialization Tests tools libs/OpenFX_extensions` prints nothing.
- The remote `fast` build and the full ctest pass against the new arena bundle.
- In the AppImage under Xvfb, the About dialog shows the GPLv3 licence, the v3 short notice and the refreshed component list, and the node menu has no Sketch, Swirl, Morphology, Roll, HaldCLUT or Wave. Share the screenshots with the user before sign-off.
- The appdata validates.
- `DECISIONS/2026-10-21-relicense-gplv3-or-later.md` is published to `docs/decisions/`.
