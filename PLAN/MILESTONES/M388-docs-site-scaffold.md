# M388 - Docs Site Scaffold

Set up the user documentation for Nucleon 1.0: an MkDocs Material project in `docs/user/`, a build script that produces both the published site and the in-app copy, a required `Docs` check on PRs, and a short getting-started section (install, a UI tour and a first comp) with screenshots captured by script. The generated node and Python references plug into the slots this milestone reserves (M389 - Node Reference Export, M390 - Python API Reference). Nothing publishes yet: that is M392 - Docs Publishing.

Context:
- **Project-wide choices** are in `DECISIONS/2026-11-10-nucleon-1-0-docs-and-site.md`: MkDocs Material, reference-only docs plus getting-started, docs versioned on the site with mike (`latest`, `1.0`, `dev`) at `https://opennucleon.github.io/docs/`, and a copy bundled into the AppImage.
- **Names.** This milestone may land before F17 - OpenNucleon Flag Day. Write the hand pages with the current names (`Natron`, `NatronRenderer`, `NatronEngine`, `.ntp`). The flag day's `tools/rename/rename.py` rewrites `docs/user/` along with everything else. URLs that are already final (`opennucleon.github.io`) are written as such.
- **The old docs** sit on the orphan `docs` branch (`git show origin/docs:Documentation/source/...`). They are 2.4-era upstream material. Use them as evidence only, never as requirements (`DECISIONS/2026-09-02-inherited-docs-are-not-requirements.md`). `guide/getstarted-*` pages are the closest prior art for getting-started.
- **Existing `docs/`** on main holds maintainer notes (`docs/cache-invalidation*`, `docs/project-format.md`) and `docs/decisions/`. The user docs live in `docs/user/` and must not pull those into the site.
- **Python in natron-dev:** `pip install --user --break-system-packages` works. CI runners use `actions/setup-python`.
- **GUI under Xvfb:** use the recipe in `build/deeprepro/run-gui.sh`, at 1920x1080. Pre-seed `checkForUpdates=false` so the first-launch dialog doesn't block. Fixtures must live under `build/`.
- **LFS:** screenshots are PNGs in `docs/user/content/img/`, stored as plain git files (not LFS), each under 300 KB (quantised with `pngquant` or saved at reduced colours if needed). No new LFS assets.

## Scope

**In:**
- `docs/user/mkdocs.yml`, `docs/user/requirements.txt` (pinned: `mkdocs-material`, `mike`, `mkdocs-awesome-pages-plugin`), and `docs/user/content/` holding the hand pages.
- Nav slots for `reference/nodes/` and `python/` that build cleanly while empty.
- `tools/docs/build.sh`, which has a site mode and an in-app mode, and takes an optional `--binary <NatronRenderer>` that M389 and M390 hook their generators into.
- A required `Docs` job in `.github/workflows/checks.yml` running a strict build of the hand pages.
- Getting-started (user, 2026-11-10): Install, UI Tour and First Comp pages.
- `tools/docs/screenshots/`, a re-runnable Xvfb capture script for the docs screenshots and for the landing page's scenes (user, 2026-11-10: scripted Xvfb captures, user approves).

**Out:**
- The generated node reference: M389 - Node Reference Export.
- The Python API and scripting reference: M390 - Python API Reference.
- Serving the docs in the app and bundling them: M391 - In-App Docs Bundle.
- Publishing, versions and the deploy key: M392 - Docs Publishing.
- Tutorials and a user guide beyond getting-started (user, 2026-11-10: reference only for 1.0).
- Re-capturing screenshots with the renamed app: M396 - Release 1.0 Cut.

**Edge cases:**
- **Empty generated sections.** Without `--binary`, `build.sh` writes a one-line placeholder `index.md` into each generated slot so `--strict` passes and the nav stays stable.
- **Maintainer docs leaking in.** `docs_dir` is `docs/user/content`, so nothing outside it is built.
- **Pages shot before the rename** show "Natron" in the title bar and splash. They are accepted for now and re-captured at M396 - Release 1.0 Cut.

**Assumptions:**
- The `Docs` check becomes required only once the user adds it to `main`'s branch protection. The PR body asks them to.

## Phase 388.1: Toolchain

- [ ] M388.P1.T1 — Create the MkDocs project and the build script
  - files: `docs/user/mkdocs.yml`, `docs/user/requirements.txt`, `docs/user/content/index.md`, `docs/user/content/.pages`, `tools/docs/build.sh`, `.gitignore`
  - approach:
    - Configure Material with `site_url: https://opennucleon.github.io/docs/`, light and dark palettes, search, `navigation.indexes`, code copy, and the awesome-pages plugin for nav from `.pages` files. Leave the logo unset for now, because the art lands with F17 - OpenNucleon Flag Day.
    - The top-level nav is Getting Started, Reference → Nodes, Python, and Releases.
    - `tools/docs/build.sh [--in-app] [--binary <renderer>] --out <dir>` copies `content/` to `build/docs/src/`, fills generated slots (placeholders when there is no binary; generator hooks are left as clearly named functions M389 and M390 fill), and runs `mkdocs build --strict`.
    - `--in-app` sets `use_directory_urls: false` and disables the external-font and analytics features so the output works offline.
    - The script installs `requirements.txt` into a venv under `build/docs/venv` if `mkdocs` is missing.
    - Gitignore `build/docs/`.
  - verify: `tools/docs/build.sh --out build/docs/site` and `tools/docs/build.sh --in-app --out build/docs/inapp` both exit 0. `build/docs/inapp/index.html` opens from `file://` with styles and search working, and loads no external URL (check with `grep -r 'https://fonts' build/docs/inapp` returning nothing).
  - size: M

- [ ] M388.P1.T2 — Add the required Docs check
  - files: `.github/workflows/checks.yml`
  - approach: Add a job named `Docs` (matching the readable naming in `DECISIONS/2026-11-09-ci-check-names.md`), on `ubuntu-latest` with `actions/setup-python`, running `tools/docs/build.sh --out build/docs/site`. Cache pip on `docs/user/requirements.txt`. Use the same trigger as the other check jobs. Pin actions the same way the existing jobs do.
  - verify: `actionlint` passes (the Lint CI job). On the PR, the `Docs` job runs and passes. A scratch commit with a broken internal link makes it fail; drop that commit afterwards.
  - size: M

## Phase 388.2: Getting Started

- [ ] M388.P2.T1 — Build the docs screenshot harness
  - files: `tools/docs/screenshots/capture.sh`, `tools/docs/screenshots/scenes/*.py`, `tools/docs/screenshots/README.md`
  - approach:
    - `capture.sh <AppImage> <outdir>` runs each scene script under Xvfb at 1920x1080 using the `build/deeprepro/run-gui.sh` recipe, with a fresh HOME and `checkForUpdates=false` pre-seeded.
    - Each scene script builds its graph from synthetic sources (Constant, CheckerBoard, ColorWheel, Text) with no external files, arranges the panes, and saves a PNG with Qt's widget grab.
    - The scenes are: the default layout with a small graph (`ui-tour`), the first-comp steps (`first-comp-1`…`-4`), and the properties panel of one node (`properties`).
    - Mind the PySide wrapper-lifetime rules for GUI scripts.
    - Quantise the output PNGs to under 300 KB each.
  - verify: `capture.sh` against a current `fast` AppImage writes every scene PNG, each non-blank and under 300 KB. A second run produces the same set of files.
  - size: M

- [ ] M388.P2.T2 — Write the Install, UI Tour and First Comp pages
  - files: `docs/user/content/getting-started/{index,install,ui-tour,first-comp}.md`, `docs/user/content/getting-started/.pages`, `docs/user/content/img/getting-started/*.png`
  - approach:
    - **Install:** the AppImage and the tarball, system requirements (Linux x86-64, glibc 2.34+, a GPU with Vulkan and OpenGL drivers), how to run each, where settings and caches live, and the `--help` of the binaries. Take the facts from `tools/release/` and `INSTALL_LINUX.md`.
    - **UI Tour:** each pane of the default layout, with the screenshot annotated in the text, not drawn on the image.
    - **First Comp:** Read → Grade → Merge over a background → Write, rendered both in the GUI and with `NatronRenderer`.
    - Agents draft the copy. It must be accurate to the current app: run every command and step it gives.
  - verify: `tools/docs/build.sh --out build/docs/site` passes `--strict`. Each command on the pages was run against the current AppImage. The rendered pages, one PNG per page from a headless browser or the built HTML, are sent to the user one file each for approval (user, 2026-11-10: agents draft, user approves).
  - size: M

- [ ] M388.P2.T3 — Add the landing-page screenshot scenes
  - files: `tools/docs/screenshots/scenes/landing-*.py`, `tools/docs/screenshots/README.md`
  - approach:
    - Add five hero and feature scenes for M393 - Landing Site, built from synthetic sources and in-app primitives only, with no external or LFS assets:
      - a keying or grading comp with a busy but tidy node graph (`landing-graph`);
      - a viewer close-up of a colour-managed result (`landing-viewer`);
      - a 3D/USD scene built from in-app geometry and lights, shown in the 3D viewer with Storm (`landing-3d`);
      - a deep composite with the deep viewer readout (`landing-deep`);
      - a GPU-node graph showing GPU placement in the node UI (`landing-gpu`).
    - Capture each at 1920x1080 through `capture.sh`, under the same rules as P2.T1.
  - verify: `capture.sh` writes the five `landing-*` PNGs, each non-blank and showing its subject. They are sent to the user one file each for an early look; final approval is at M393 - Landing Site, on the renamed app.
  - size: M

**Verification gate:**
- `tools/docs/build.sh` passes in both modes, and the `Docs` check is green on the PR.
- The in-app build loads no external URL.
- The user has approved the getting-started pages from the rendered screenshots sent one file each.

## Decisions

- 2026-11-10 — Getting-started covers Install, UI Tour and First Comp: enough to orient someone coming from Natron or a similar compositor (user).
- 2026-11-10 — A strict build of the hand pages is a required PR check; the generated reference is checked where a binary exists (user).
- 2026-11-10 — Hand pages use the current names until F17 - OpenNucleon Flag Day: the rename script rewrites `docs/user/` with the rest of the tree, so the commands stay runnable before the flag day (planner).
- 2026-11-10 — The screenshot harness lives in `tools/docs/screenshots/` and also serves M393 - Landing Site, so one script captures every public screenshot for each release (planner).
