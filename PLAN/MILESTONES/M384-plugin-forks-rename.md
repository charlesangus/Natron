# M384 - Plugin Forks Rename

Rename Natron to Nucleon throughout the OFX plug-in forks the app bundles. Their plug-in IDs (`fr.inria.openfx.*`), the OFX Support library's `isNatron` host check and its Natron extension properties (`kNatronOfx*`) all change, and each fork's remaining Natron mentions go too (user, 2026-11-10). The work sits on a `opennucleon-rename` branch in each fork. Nothing merges into a fork's default branch here, because other milestones keep pinning those default branches until the flag day. In this repo the milestone adds only a manifest of those branches. M385 - OpenNucleon Code Rename rebases the branches, merges them and pins them.

Context:
- **Forks** (from `tools/ci/local/fetch-assets.sh` and `.gitmodules` on `main`):
  - `charlesangus/openfx-io`: `OPENFX_IO_REF`
  - `charlesangus/openfx-misc`: `OPENFX_MISC_REF`
  - `charlesangus/openfx-arena`: `OPENFX_ARENA_REF`
  - `charlesangus/openfx-supportext`
  - `charlesangus/openfx-natron`: the `libs/OpenFX` submodule
  - Each plug-in repo also carries its own `openfx` (and possibly `SupportExt`) submodule. Which fork each one points at is P1.T1's to find out.
- **Host name.** The new OFX host name is `io.github.opennucleon.Nucleon`. The Support library's Natron check (named `isNatron` in NatronGitHub/openfx) becomes `isNucleon` and matches only the new name.
- **Extension properties.** `kNatronOfx*` property names and their string values are shared between the host (`libs/OpenFX_extensions/`, which M385 - OpenNucleon Code Rename renames through `tools/rename/rename.py`) and the plug-ins. Both sides must use the same new strings. Take them from `rules.toml`, applied with `rename.py --root <fork>`.
- **Repo transfer.** The user transfers the forks to the `opennucleon` org and renames `openfx-natron` → `openfx-nucleon` before the flag day (see the F17 - OpenNucleon Flag Day open question). Work here uses the current `charlesangus/*` URLs; GitHub redirects them after the transfer.

## Scope

**In:**
- An `opennucleon-rename` branch on each of the five forks, and on any further fork that a plug-in's openfx submodule points at, with every Natron mention renamed apart from preserved copyright and credit lines.
- Each plug-in's openfx submodule pointing at the renamed Support library branch.
- `tools/rename/forks.toml` in this repo, listing each fork, its branch and the head SHA.

**Out:**
- Merging into any fork's default branch, and bumping the pins in this repo: M385 - OpenNucleon Code Rename.
- Transferring or renaming GitHub repos: a user action before the flag day.
- Plug-ins the app does not bundle, such as openfx-gmic.

**Edge cases:**
- **Upstream openfx submodule.** If a plug-in's openfx submodule points at `NatronGitHub/openfx` rather than a charlesangus fork, fork it as `charlesangus/openfx-natron` (if that is the same code) or a new fork. Record which in `## Decisions`.
- **Fork default branches keep moving.** Other milestones' fork-and-fix PRs may land on a fork's default branch after this milestone. M385 - OpenNucleon Code Rename rebases each rename branch at the flag day and re-runs `rename.py --root`.
- **Plug-in IDs inside the plug-ins' own presets and tests** are renamed like every other mention.

**Assumptions:**
- `rename.py --root` from M383 - Rename Script And Guard handles the forks' trees with the same rules, plus a per-fork allowlist where needed.

## Phase 384.1: Survey

- [ ] M384.P1.T1 — Map where each fork defines the host check, the extensions and the plug-in IDs
  - files: read-only: the five forks at their pinned SHAs, plus their submodules
  - approach: For each fork, record its submodule URLs and SHAs, the file and line where the Natron host-name comparison and `isNatron` are defined, which files use `kNatronOfx*`, the `fr.inria.*` plug-in IDs, and a count of other Natron mentions. Write the findings into this milestone's `## Decisions`.
  - verify: Every fork and submodule is covered. `isNatron` has exactly one defining site per Support-library copy.
  - size: S

## Phase 384.2: Rename Branches

- [ ] M384.P2.T1 — Rename the OFX Support library forks
  - files: `charlesangus/openfx-natron` and any further openfx or SupportExt fork P1.T1 found: the `opennucleon-rename` branch
  - approach: Branch `opennucleon-rename` off each fork's pinned SHA and run `rename.py --root <fork>`. Hand-check the host-name comparison so `isNucleon` matches only `io.github.opennucleon.Nucleon`. Build the Support library the way `fetch-assets.sh` builds the plug-ins against it. Push the branch, but open no PR into the default branch yet.
  - verify: `grep -rni 'natron\|fr\.inria'` on the branch shows only allowlisted lines, and the Support library compiles.
  - size: M

- [ ] M384.P2.T2 — Rename openfx-io, openfx-misc and openfx-arena
  - files: `charlesangus/openfx-{io,misc,arena}`: the `opennucleon-rename` branch of each
  - approach: Branch off each pinned SHA, point the openfx and SupportExt submodules at P2.T1's branches, and run `rename.py --root <fork>`. Plug-in IDs `fr.inria.openfx.*` become `io.github.opennucleon.openfx.*`. Build each bundle with the `fetch-assets.sh` recipe, then push the branches.
  - verify: Each bundle builds. `strings <bundle>.ofx | grep -c 'fr\.inria'` prints 0, and the grep from P2.T1 is clean.
  - size: L

- [ ] M384.P2.T3 — Record the branch manifest
  - files: `tools/rename/forks.toml` (new)
  - approach: One entry per fork: repository URL, rename branch, head SHA, the default-branch SHA it was cut from, and the variable or submodule in this repo that pins it (`OPENFX_IO_REF`, `libs/OpenFX`, …).
  - verify: Every SHA in the file resolves with `git ls-remote` against its fork.
  - size: S

**Verification gate:**
- Every fork's `opennucleon-rename` branch builds, as checked in P2.T1 and P2.T2.
- `forks.toml` SHAs resolve.
- No fork's default branch changed.
- The PR into `feature/f17-opennucleon-flag-day` contains only `tools/rename/forks.toml`.
