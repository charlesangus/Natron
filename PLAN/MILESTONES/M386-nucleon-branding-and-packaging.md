# M386 - Nucleon Branding And Packaging

Put the Nucleon face on the renamed app. Wire M382 - Nucleon Logo And Artwork's art set into the resources, remove the Natron art, lay out the splash text over the new art's reserved bands, redo the About dialog with a "fork of Natron" credit, and rename the desktop integration and the AppImage. The README, CONTRIBUTING and INSTALL guides are rewritten as OpenNucleon. Website, update-check and issue-tracker URLs move to the `opennucleon` org.

**Held:** starts only at the user's go-ahead, as part of F17 - OpenNucleon Flag Day.

Context:
- **Art set:** `Gui/Resources/Branding/` and its `README.md`, from M382 - Nucleon Logo And Artwork. The README gives each file's role, the splash text-band rectangles and the palette.
- **Code that draws the splashes:** `Gui/SplashScreen.cpp` (startup text at (120, 100) and (20, h − 15); project splash at (250, h − 51) and (300, h/2)). `Gui/GuiApplicationManager.cpp` constructs the splash from `NUCLEON_IMAGES_PATH "splashscreen.png"`, after M385 - OpenNucleon Code Rename has renamed the macro.
- **About dialog:** `Gui/AboutWindow.cpp`. It shows the logo at 128 px (line ~71), the window title "About %1", and links to NatronGitHub and the openfx repos (lines ~142-403).
- **Desktop integration:** `Gui/Resources/Applications/*.desktop`, `Gui/Resources/Metainfo/*.appdata.xml`, `Gui/Resources/Mime/*.xml`, installed by `App/CMakeLists.txt:74-82`. The packaging scripts are `tools/release/make-appimage.sh`, `make-tarball.sh` and `stage-bundle.sh`. M385's script has already renamed the identifiers in all of these. This milestone fixes their content, the art references and the IDs.
- **URLs** (`Global/Macros.h`): the website and `LATEST_VERSION.txt` become `https://opennucleon.github.io/`, the issue tracker becomes `https://github.com/opennucleon/OpenNucleon/issues`, and the forum and readthedocs links are dropped until replacements exist (user, 2026-11-10).

## Scope

**In:**
- Art wired into `Gui/GuiResources.qrc`, with HiDPI 2x variants picked on scale > 1.
- The old Natron images deleted.
- Splash text layout, the About dialog, the window icon and the desktop, AppStream and MIME files with ID `io.github.opennucleon.Nucleon`.
- AppImage and tarball names `Nucleon-<ver>-x86_64`.
- README, CONTRIBUTING, INSTALL_LINUX.md, CONTRIBUTORS.txt and the `.github/` issue and PR templates.

**Out:**
- Designing art: M382 - Nucleon Logo And Artwork.
- Creating the `opennucleon.github.io` site repo and its `LATEST_VERSION.txt`: a user action before the flag day.
- The dev image and CI infrastructure: M387 - Dev Infrastructure Rename.

**Edge cases:**
- **Update check before the site exists.** If `opennucleon.github.io/LATEST_VERSION.txt` returns 404, the check must fail quietly, as it does on a network error today. Verify that path.
- **Dropped links.** Remove the forum and documentation menu entries or buttons rather than leaving dead links. `NATRON_DOCUMENTATION_ONLINE` (renamed to `NUCLEON_…`) loses its online URL; local docs still work.
- **Icon theme lookup.** The desktop file's `Icon=` must match the installed icon file name, and the AppImage's top-level `.DirIcon` must match too.

**Assumptions:**
- Unconfirmed: the About dialog keeps its existing tab layout; only the logo, text and links change.

## Phase 386.1: Art In The App

- [ ] M386.P1.T1 — Swap the art in the resources and delete the Natron images
  - files: `Gui/GuiResources.qrc`, `Global/Macros.h` (the app icon path), `Gui/Resources/Images/` (deletions), `Gui/Resources/Branding/`
  - approach:
    - Add the Branding PNGs to the qrc under stable aliases: `splashscreen.png`, `loadProjectSplashscreen.png`, `nucleonIcon256.png`, `nucleonProjectIcon256.png`, plus the `@2x` variants.
    - Point the app-icon macro at the new icon.
    - Delete `splashscreen.{png,svg}`, `loadProjectSplashscreen.png`, the `*Icon*` Natron files (`.png`, `.svg`, `.pdf`, `.icns`, README) and the two unused `natron_picto_*` SVGs, after grepping that nothing references them.
  - verify: A `fast` build succeeds, and `rename.py --check` passes. Nothing in the qrc points at a deleted file (`rcc` would fail the build).
  - size: M

- [ ] M386.P1.T2 — Lay out the splash text over the new art
  - files: `Gui/SplashScreen.cpp`, `Gui/SplashScreen.h`
  - approach: Load the `@2x` pixmap when the device pixel ratio is above 1. Replace the hardcoded text coordinates with the band rectangles from `Branding/README.md`, drawn with `drawText(QRectF, flags, text)`, clipped and elided. Keep the existing font and size unless the README says otherwise. Use the same approach for `LoadProjectSplashScreen`.
  - verify: Under Xvfb with the AppImage, screenshot the startup splash and the loading-project splash (open a `.nucz`) at scale 1 and at `QT_SCALE_FACTOR=2`, and send them to the user one file each.
  - size: M

- [ ] M386.P1.T3 — Redo the About dialog
  - files: `Gui/AboutWindow.cpp`
  - approach: Use the About logo at 128 px (256 px on HiDPI) and "About Nucleon" as the title. The text says "Nucleon, part of OpenNucleon", followed by a line "OpenNucleon is a fork of Natron (https://github.com/NatronGitHub/Natron)". Repoint the links to `github.com/opennucleon/*` repos (OpenNucleon, openfx-io, openfx-misc, openfx-arena). Drop openfx-gmic and any other link the app no longer ships.
  - verify: An Xvfb screenshot of each About tab is sent to the user. The fork credit is present.
  - size: M

## Phase 386.2: Desktop And Packaging

- [ ] M386.P2.T1 — Rename the desktop, AppStream and MIME files
  - files: `Gui/Resources/Applications/`, `Gui/Resources/Metainfo/`, `Gui/Resources/Mime/`, `App/CMakeLists.txt`
  - approach:
    - File names become `io.github.opennucleon.Nucleon.{desktop,appdata.xml}` and the MIME XML `opennucleon-project.xml`.
    - Desktop file: `Name=Nucleon`, `Exec=Nucleon %U`, `Icon=io.github.opennucleon.Nucleon`, `MimeType=application/vnd.opennucleon.project`.
    - AppStream: id, name, summary, description (which mentions it is a fork of Natron), homepage, bugtracker, and a screenshot URL (drop it if none exists).
    - MIME XML: the type, the comment "Nucleon Project", the glob `*.nucz` and the icon.
    - Install the icon set at every size into `share/icons/hicolor/<n>x<n>/apps/` and `.../mimetypes/`.
  - verify: `desktop-file-validate` and `appstreamcli validate` pass (`xmllint --noout` if either tool is missing). `cmake --install` into a temp prefix lays out the hicolor tree.
  - size: M

- [ ] M386.P2.T2 — Rename the AppImage and the tarball
  - files: `tools/release/make-appimage.sh`, `tools/release/make-tarball.sh`, `tools/release/stage-bundle.sh`, `tools/ci/local/package.sh`
  - approach: Rename the AppDir to `Nucleon.AppDir` and the bundles to `Nucleon-${VERSION}-x86_64.AppImage` and `Nucleon-${VERSION}-linux-x86_64.tar.*`. Point the AppRun exec at `bin/Nucleon` and use the new desktop, icon and appdata names, with `.DirIcon` from the 256 px icon. Update any CI step or `tools/ci/remote/build.sh` glob that matches `Natron-*.AppImage`; `rename.py` covers most of these, so check the result.
  - verify: `tools/ci/local/package.sh` (or the remote `fast` build) produces `Nucleon-*.AppImage`. `tools/release/check-startup.sh` and `check-relocatable.sh` pass on it. The AppImage's embedded desktop file passes `desktop-file-validate`.
  - size: M

## Phase 386.3: Docs And URLs

- [ ] M386.P3.T1 — Point the URLs at the opennucleon org and drop the dead links
  - files: `Global/Macros.h` (website, latest-version, issue-tracker, forum and docs URL macros), `Engine/AppInstance.cpp` (the update-check and download link), `Gui/` menu entries that use the forum or online docs macros
  - approach: Set the URLs per Context. Remove the forum macro and its menu entry, and the online-docs URL and its menu entry, leaving the local-docs path working. Check that a 404 on the update check is handled as quietly as a network error.
  - verify: `rename.py --check` passes. Under Xvfb with checking enabled, a 404 from a stubbed URL shows no dialog (point the macro at a 404 URL in a scratch build, or use `NUCLEON_LATEST_VERSION_URL` if it can be overridden). The Help menu has no dead entries (screenshot).
  - size: M

- [ ] M386.P3.T2 — Rewrite the README and contributor docs as OpenNucleon
  - files: `README.md`, `CONTRIBUTING.md`, `INSTALL_LINUX.md`, `CONTRIBUTORS.txt`, `.github/ISSUE_TEMPLATE/*.yml`, `.github/PULL_REQUEST_TEMPLATE.md`
  - approach: In the README, use the title "OpenNucleon" and the app name "Nucleon", add the new logo (the lockup PNG from Branding), and a short "OpenNucleon is a fork of Natron" section linking upstream and crediting its developers. Fix the badges and links to `opennucleon/OpenNucleon`. Keep the GPLv3 wording from M157 - Relicense To GPLv3 Or Later. Point the issue templates at the new repo. Keep the Natron contributors in `CONTRIBUTORS.txt` under a "Natron contributors" heading.
  - verify: `rename.py --check` passes. The README renders on the PR's GitHub view: check the logo image and the badges.
  - size: M

**Verification gate:**
- The remote `fast` build and full ctest pass.
- `rename.py --check` exits 0.
- The packaged `Nucleon-*.AppImage` passes `check-startup.sh` and `check-relocatable.sh`.
- `desktop-file-validate` and `appstreamcli validate` pass.
- Xvfb screenshots (startup splash at scale 1 and 2, loading splash, every About tab, the window icon in the title bar, the Help menu) are sent to the user one file each. They are collected at the F17 - OpenNucleon Flag Day UAT.
- The PR targets `feature/f17-opennucleon-flag-day`.
