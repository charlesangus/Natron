# Nucleon 1.0: website, docs and release shape (2026-11-10)

The user set the shape of the 1.0 release work (E21 - Nucleon 1.0 Release), on top of the OpenNucleon rename (F17 - OpenNucleon Flag Day):

- **Release:** the first stable release is Nucleon 1.0.0. Betas lead up to it as `v1.0.0-betaN` and remain a rolling channel, one per green `main` push, pruned to the newest ten. 1.0 goes through an RC and the user's UAT. Which Features must land first is the user's call, made later.
- **Website:** a landing page at `https://opennucleon.github.io/` (hero and screenshots, feature overview, download). It is built with Astro, and its source lives in the `opennucleon/opennucleon.github.io` repo.
- **Docs:** a separate docs site at `https://opennucleon.github.io/docs/`, built with MkDocs Material from `docs/user/` in OpenNucleon. Versions are published with mike: `latest`, a frozen `<major>.<minor>` per stable release, and `dev` from betas.
  - 1.0 docs are reference only: the generated node reference, a Python API reference generated from the bindings plus hand descriptions (a CI check fails on any gap), scripting reference pages, and a short getting-started. There is no user guide or tutorials.
  - The inherited Sphinx docs on the orphan `docs` branch serve as evidence only. That branch and the 2014 `gh-pages` branch are deleted after 1.0, once archive tags exist.
- **In-app help:** the docs are bundled into the AppImage and the tarball and served offline. The online source points at `/docs/latest/`.
- **Release notes:** hand highlights plus a full changelog grouped by epic for 1.0. Later stable releases get the grouped changelog generated automatically.
- **Prose:** agents draft the getting-started pages, landing copy and highlights, and the user approves each one.
- **Sequencing:** the docs scaffold, node reference and Python reference can land before the flag day. Everything that needs the new names, art, org or site repo follows the F17 landing.

Planner's technical calls:
- The site repo's `gh-pages` branch is shared. The landing workflow owns the root, OpenNucleon's `docs.yml` owns `docs/`, and `LATEST_VERSION.txt` is written for stable releases only.
- Docs are built from the released AppImage, so they document the exact binary users download.
- Download links are fetched in the browser from the Releases API, with a static fallback.
