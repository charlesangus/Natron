# Rename to OpenNucleon / Nucleon (2026-11-10)

The project becomes **OpenNucleon** and the app **Nucleon** (user, 2026-11-10). This supersedes `docs/decisions/2026-08-29-keep-natron-branding.md`. The rename is total, code included. It lands as F17 - OpenNucleon Flag Day together with M157 - Relicense To GPLv3 Or Later, at a quiet time the user chooses, because both conflict with every open branch.

Mapping, which `tools/rename/rules.toml` implements:
- Names: `Natron` → `Nucleon`, `NATRON` → `NUCLEON` and `natron` → `nucleon` everywhere, including the C++ namespace, every `NATRON_*` macro, include guards, class prefixes, CMake targets and options, and environment variables.
- Organisation and identity:
  - Organisation "INRIA" becomes "OpenNucleon"; the reverse domain `fr.inria` becomes `io.github.opennucleon`.
  - Plug-in IDs: `fr.inria.openfx.*` → `io.github.opennucleon.openfx.*`, and the PyPlugs' `fr.inria.X` → `io.github.opennucleon.X`. No `fr.inria` ID may remain.
  - OFX host name: `io.github.opennucleon.Nucleon`. The plug-in forks are patched to match, including `isNatron` → `isNucleon` and `kNatronOfx*` → `kNucleonOfx*`.
  - Desktop and AppStream ID: `io.github.opennucleon.Nucleon`. MIME type: `application/vnd.opennucleon.project`.
- Binaries: `Nucleon`, `NucleonRenderer` and `nucleon-python`. The AppImage is `Nucleon-<ver>-x86_64.AppImage`.
- Python: modules `NucleonEngine` and `NucleonGui`, and the global object `nucleon`.
- Extensions: `.ntpz` → `.nucz`, `.ntc` → `.nucc`, `.nl` → `.nucl`, `.nps` → `.nucp`.
- Paths: settings at `~/.config/OpenNucleon/Nucleon.conf`, caches under `~/.cache/Nucleon/`. Nothing is migrated, since the fork keeps no backward compatibility.
- Repos and URLs:
  - The GitHub org is `opennucleon`. The repo becomes `opennucleon/OpenNucleon`.
  - The forks become `opennucleon/openfx-{io,misc,arena,supportext}`, and `openfx-natron` becomes `openfx-nucleon`.
  - The website and update check move to `https://opennucleon.github.io/`.
  - The forum and readthedocs links are dropped until replacements exist.
  - The user does the transfers before the flag day.
- Dev infrastructure: image `ghcr.io/opennucleon/nucleon-dev`, container `nucleon-dev`, and `skills/nucleon-maintainer`.
- Credit:
  - Licence headers read "This file is part of OpenNucleon <https://opennucleon.github.io/>". They keep the existing INRIA and Natron-developers copyright lines and add "(C) 2026 The OpenNucleon developers".
  - The About dialog and the README say "OpenNucleon is a fork of Natron".
  - `docs/decisions/` history is not rewritten.
- Art: four agent-drafted SVG concepts (nucleus, orbital ring, N monogram, node graph), from which the user picks. The splash shows the mark and the wordmark on a dark background.
- Guard: after the landing, CI and the pre-commit hook reject new `natron` or `fr.inria` mentions outside an allowlist (`tools/rename/rename.py --check`).
