# Milestone 14: Sphinx documentation tree and doc CI

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Make the inherited `Documentation/` tree either correct and published, or gone.
Today it is neither: ~70 pages of upstream 2.4-era user guide that no workflow
builds, no gate verifies, and no site publishes — while `.readthedocs.yaml`
exists and `README.md` carries an RTD badge resolving to *upstream's* project.
This milestone owns both halves at once, because the answer to "how much of
this do we fix" depends entirely on whether it will be published and gated.
Split out of M6 on 2026-09-03 (the user's call; see that milestone's
`## Decisions`) once it became clear this is a body of work, not a task.

Blocked on: an unanswered question — whether this fork publishes user
documentation at all (fork-owned Read the Docs project, GitHub Pages, or
in-tree only). The three plausible approaches — repair the guide, replace it,
or delete `Documentation/` and `.readthedocs.yaml` outright under
`docs/decisions/2026-09-02-inherited-docs-are-not-requirements.md` — are
mutually exclusive and produce entirely different task sets. See
`# Open questions` on the board.

Acceptance sketch:
- A doc build either runs in CI and gates merges, or `Documentation/` and
  `.readthedocs.yaml` no longer exist. No third state.
- If the tree survives: no page describes a platform this fork does not build,
  a plugin it does not ship, or an installer it does not publish. Known
  offenders found while scoping M6, each verified against the tree on
  2026-09-03:
  - `guide/getstarted-installation-{mac,windows}.rst` — live, toctree-reachable
    dead-platform install chapters (Windows 7/8/10; "Mac OS X 10.6 or higher"),
    plus 13 wizard screenshots under `guide/_images/`.
  - `guide/getstarted-installation-linux.rst` — documents a graphical installer
    (`:41`), a `NatronSetup` maintenance tool (`:104`) and RPM/DEB packages
    (`:119`), none of which this fork produces; distro list (`:18-23`) predates
    the EL9 baseline.
  - `guide/getstarted-about-faq.rst:22-23,79-91` — an OS-support matrix listing
    Windows 7/8/10 and "MacOSX 10.6 or greater". **The user asked on 2026-09-03
    that this one be fixed; carry it into whatever shape this milestone takes.**
  - `guide/tutorials-svgworkflow.rst` — an end-to-end workflow on the `ReadSVG`
    node, which is not built (`docs/decisions/2026-09-02-build-openfx-arena.md:11-13`).
  - `guide/getstarted-about-features.rst` — lists OpenFX-G'MIC, OpenCV, Yadif,
    Vegas and TuttleOFX as supported; claims SVG among supported formats;
    Retina/32-bit claims; a "Version 2.1 will incorporate the Tracker from
    Blender" future-tense claim.
  - `guide/getstarted-troubleshooting.rst:68` — advises switching to Linux or
    macOS to dodge a Windows-only problem.
  - `source/_environment.rst:30-96,124` — roughly half the page is Windows and
    macOS instructions. Note this is the one `_`-prefixed file that is *not*
    generated (`tools/genStaticDocs.sh:43` regenerates only `_group*`,
    `_prefs.rst` and `plugins/`), so it is safe to hand-edit despite the blanket
    warning at `Documentation/README-Natron-documentation.md:6`.
  - `guide/tutorials-writedoc.rst:40,41,47,57,100` — sends contributors to
    upstream, tells them to branch from `RB-2.4`, and lists `openfx-gmic`.
  - `Documentation/README-Natron-documentation.md:12` — gives the
    `genStaticDocs.sh` invocation as a macOS `/Applications/Natron.app` example.
  - `guide/tutorials-hugin.md` — a byte-for-byte Markdown duplicate of the
    `.rst` that Sphinx never reads (`conf.py:34` sets `source_suffix = '.rst'`).
  - `guide/getstarted-troubleshooting.rst:32,34,43,55,60,61` and
    `guide/tutorials-writedoc.rst:36,102` — route users and doc contributors to
    **upstream's** issue tracker. This fork uses its own (see M6's
    `## Decisions`, 2026-09-03); found while fixing the root docs and left here
    because M6 does not own `Documentation/`.
- No dangling toctree entries. There are exactly two today —
  `known-bugs-and-workarounds`, referenced from
  `guide/getstarted-troubleshooting.rst:83` and `guide/getstarted.rst:15`, with
  no such file anywhere under `Documentation/`.
- `.readthedocs.yaml` is coherent with what CI does. Today it requests
  `python: "3.12"` (`:12`) while `Documentation/source/requirements.txt` pins
  `docutils<0.18` and `sphinx-rtd-theme==1.0.0`, which likely will not install
  there — the config is probably broken, not merely unused.
- `.github/workflows/nightly.yml:29` sets `paths-ignore: - Documentation`, which
  as written matches only a file literally named `Documentation`; the intended
  filter is `Documentation/**`. Doc-only pushes to `main` currently trigger the
  nightly. Harmless but wrong, and this milestone will trip it repeatedly.
