# M391 - In-App Docs Bundle

Ship the docs inside the AppImage and the tarball, and make in-app help use them. Release and UAT packaging builds the in-app variant of the docs with the binary it just built, and stages it at `Resources/docs/html`. The local documentation server serves that static MkDocs output in place of the old Sphinx tree. Help buttons open the bundled page for the node, or the preferences page, and the "online" documentation source points at `https://opennucleon.github.io/docs/latest/`.

Context:
- **Runs after F17 - OpenNucleon Flag Day** (Deps M386 - Nucleon Branding And Packaging), so the names here are the renamed ones: `Nucleon`, `NucleonRenderer`, `NUCLEON_*` macros. M386.P3.T1 removed the online-docs URL and its menu entry because no site existed. This milestone restores them, pointing at the new site.
- **Local doc server:** `Gui/DocumentationManager.cpp`.
  - The docs directory is `<appBinaryPath>/../Resources/docs/html/` (`:118`).
  - Virtual pages are built live: `_plugin.html?id=`, `_group.html?id=`, `_prefs.html` and `_prefsLive.html`. A static file takes precedence when it exists (`:163-165`).
  - `parser(html, docDir)` (`:520`) is Sphinx-specific. It pulls the `toctree-wrapper` block out of `index.html`, rewrites `href`s and injects that menu. It also replaces `NATRON_DOCUMENTATION` (renamed by F17) and `__REPLACE_TITLE__`.
  - The generated pages link Sphinx `_static/{default.css,pygments.css,style.css,jquery.js,dropdown.js}` (`:254-258`, `:286`, `:480`).
  - When the directory is missing, pages fall back to a 404 body or unstyled HTML, without crashing.
- **Help entry points:**
  - `Gui::openHelpDocumentation` (`Gui/Gui.cpp:843`) opens the root.
  - `DockablePanel::showHelp` (`Gui/DockablePanel.cpp:950`) opens `<online>/plugins/<id>.html`.
  - `PreferencesPanel::openHelp` (`Gui/PreferencesPanel.cpp:1232`) opens the root online and `/_prefs.html` locally.
  - The "Documentation Source" preference (`Engine/Settings.cpp:714-726`, default 0) is local, online or none. Local falls back to online when the server port is 0.
- **Packaging:** `tools/release/stage-bundle.sh` stages `bin/`, `lib/`, `plugins/` and `Resources/` beside each other, and installs no docs today. `tools/ci/local/package.sh` builds the UAT AppImages through it, and `.github/workflows/release.yml` builds releases in the dev image.
- **Docs layout** from M389 - Node Reference Export and M390 - Python API Reference: node pages at `reference/nodes/<pluginID>.html` in the in-app build (`use_directory_urls: false`), preferences at `reference/preferences.html`, and the Python pages under `python/`.

## Scope

**In:**
- Staging the in-app docs in `stage-bundle.sh` for releases and UAT packages.
- Replacing the Sphinx-specific server logic. Static pages are served as-is. Live pages (`_plugin.html` for plug-ins with no bundled page, `_prefsLive.html`) are wrapped in a small template that links the bundled Material CSS.
- Help-button and menu URLs for both the local and the online source.
- The online source URL `https://opennucleon.github.io/docs/latest/`.

**Out:**
- Building, publishing and versioning the online site: M392 - Docs Publishing.
- Writing docs content: M388 - Docs Site Scaffold, M389 - Node Reference Export and M390 - Python API Reference.
- Changing the Documentation Source preference's choices or its default (local).

**Edge cases:**
- **Third-party plug-ins and user PyPlugs** have no bundled page. `_plugin.html?id=` falls back to the live-generated page in the template.
- **Docs missing from the bundle** (a dev build run from the build tree). The server shows a one-line page saying local docs aren't installed, with a link to the online docs, instead of a bare 404.
- **Live preferences page.** `_prefsLive.html` stays live, because it shows the current values. The static `reference/preferences.html` is the default page when no live values are needed.
- **Plug-in ID to page mapping.** Online help uses `<site>/reference/nodes/<pluginID>/` (directory URLs). Local help uses `reference/nodes/<pluginID>.html`.
- **Server port 0** keeps its current fallback to the online source.

**Assumptions:**
- Unconfirmed: the Help menu keeps its existing entries, repointed (Documentation; Python reference if an entry exists today), and gains no new ones.
- mkdocs can be pip-installed into the release container at packaging time. The runners have network access. Locally, `pip --user --break-system-packages` works in nucleon-dev.

## Phase 391.1: Bundle

- [ ] M391.P1.T1 — Stage the in-app docs in release and UAT bundles
  - files: `tools/release/stage-bundle.sh`, `tools/ci/local/package.sh`, `.github/workflows/release.yml`
  - approach:
    - After staging `bin/`, run `tools/docs/build.sh --in-app --binary "$STAGE/bin/NucleonRenderer" --out "$STAGE/Resources/docs/html"`.
    - Add a `--no-docs` flag (or `NUCLEON_STAGE_NO_DOCS=1`) for quick local packages. It is off by default and is never passed in release.yml.
    - Install the docs requirements in the release job, before packaging, with pinned pip.
  - verify: `tools/ci/local/package.sh` produces an AppImage whose extracted `Resources/docs/html/index.html`, `reference/nodes/` and `python/api/` exist. The AppImage size increase is reported in the PR body.
  - size: M

## Phase 391.2: Server And Help

- [ ] M391.P2.T1 — Serve the MkDocs output from the local doc server
  - files: `Gui/DocumentationManager.cpp`, `Gui/DocumentationManager.h`, `Gui/Resources/docs-template.html` (new, registered in `Gui/GuiResources.qrc`)
  - approach:
    - Serve files under `docDir` directly with the right MIME types, mapping `/` to `index.html`.
    - Map `_plugin.html?id=<id>` to `reference/nodes/<id>.html` when it exists. Otherwise render the live `genHTML=true` page in the template.
    - Keep `_prefsLive.html` live, in the template.
    - Delete the Sphinx toctree parser, the `_group.html` live pages (group pages are static now), and the old `_static` references.
    - Show the "local docs not installed" page when `docDir` is missing.
  - verify: Under Xvfb with the P1.T1 AppImage, fetch (with curl against the server port) `/`, a native node page, an OFX node page, `/_plugin.html?id=<a scratch PyPlug>`, `/_prefsLive.html` and a Python API page. Each returns 200 with Material CSS linked. Run the same check with `Resources/docs` removed: the page shows the not-installed message.
  - size: M

- [ ] M391.P2.T2 — Repoint the help buttons and the online source
  - files: `Global/Macros.h` (the online docs URL macro), `Gui/Gui.cpp` (`openHelpDocumentation`), `Gui/DockablePanel.cpp` (`showHelp`), `Gui/PreferencesPanel.cpp` (`openHelp`), the Help menu entry that M386 - Nucleon Branding And Packaging removed
  - approach:
    - Set the online URL to `https://opennucleon.github.io/docs/latest/`.
    - Node help opens `reference/nodes/<id>.html` locally, and `<online>reference/nodes/<id>/` online.
    - Preferences help opens `_prefsLive.html` locally and `<online>reference/preferences/` online.
    - Restore the Help → Documentation menu entry, honouring the source preference.
  - verify: Under Xvfb, with the local source, the node "?" button, the preferences help and Help → Documentation each open the expected local URL: capture the URL handed to `QDesktopServices` through a test hook or logging. With the online source, each URL matches the site layout from M392 - Docs Publishing's nav. Screenshots of the rendered local node page and the Help menu are sent to the user one file each.
  - size: M

**Verification gate:**
- The remote `fast` build and full ctest pass.
- A packaged AppImage contains the docs and serves them offline. The P2.T1 curl checks pass against that AppImage under Xvfb.
- The help buttons open the right local and online URLs.
- Screenshots of a local node help page, the Python reference page and the Help menu are sent to the user one file each.

## Decisions

- 2026-11-10 — Docs are bundled into the AppImage and the tarball and served offline. The "online" source points at the site's `latest` docs (user).
- 2026-11-10 — The local server serves the MkDocs output as static files and wraps only the truly live pages (unbundled plug-ins, live preferences) in a template: the Sphinx toctree-injection parser has nothing to parse in Material's output (planner).
