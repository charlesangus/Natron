# M392 - Docs Publishing

Publish the docs at `https://opennucleon.github.io/docs/`, versioned with mike. Every beta publishes `dev`. A stable release publishes its `<major>.<minor>` version, moves the `latest` alias to it, and writes the update-check file `LATEST_VERSION.txt` at the site root. A workflow in OpenNucleon builds the docs from the released AppImage, so nothing is compiled twice, and pushes them into the `opennucleon/opennucleon.github.io` repo's `gh-pages` branch under `docs/`.

Context:
- **Runs after F17 - OpenNucleon Flag Day** and M394 - Release Versioning, so it uses the renamed binaries and the `v<major>.<minor>.<patch>[-betaN|-rcN]` tag scheme.
- **Site repo layout** (`DECISIONS/2026-11-10-nucleon-1-0-docs-and-site.md`): `opennucleon/opennucleon.github.io` serves Pages from its `gh-pages` branch. Three writers share that branch, each owning one part:
  - The site repo's own workflow (M393 - Landing Site) owns the root.
  - This milestone's mike deploys own `docs/`.
  - This milestone also owns `LATEST_VERSION.txt` at the root.
  Each writer must leave the other parts untouched.
- **Release flow:** `beta-release.yml` tags each green `main` push and dispatches `release.yml`, which builds in the dev image and runs `gh release create … --generate-notes`. A tag containing `-` is marked prerelease (`release.yml:~203`). M394 - Release Versioning sets the names and statuses.
- **Update-check format** (`Engine/AppInstance.cpp:451-495`) is line-prefixed:
  - `File version: 1`
  - `Software version: x.y.z` (exactly three numbers)
  - `Development status: <Release|RC|Beta|…>`
  - `Build number: <n>`

  The app compares the encoded version first, then the development-status rank, then the build number.
- **Docs build:** `tools/docs/build.sh --binary <renderer> --out <dir>` from M388 - Docs Site Scaffold, with the generators from M389 - Node Reference Export and M390 - Python API Reference. An AppImage's renderer can be run with `--appimage-extract-and-run` or by extracting it.

## Scope

**In:**
- `.github/workflows/docs.yml`, triggered by a published release and by manual dispatch with a tag.
- mike deploys with `--deploy-prefix docs` to the site repo through a deploy key.
- Version mapping:
  - a prerelease publishes `dev`;
  - stable `vX.Y.Z` publishes `X.Y` and moves `latest` to it;
  - the default is `latest`;
  - before the first stable release the default is `dev`.
- `LATEST_VERSION.txt` written on stable releases only.
- A mike version picker in the site.

**Out:**
- Landing page content and its workflow: M393 - Landing Site.
- The in-app copy: M391 - In-App Docs Bundle.
- Creating the site repo, enabling Pages and adding the deploy key: user actions (see `# Open questions`).

**Edge cases:**
- **Betas arriving faster than deploys.** The concurrency group `docs-dev` uses `cancel-in-progress: true`. Stable deploys use their own group and are never cancelled.
- **Patch releases.** `v1.0.1` redeploys `1.0` and keeps `latest` on it.
- **A stable deploy failing after the release is published.** The workflow can be re-dispatched with the tag, and a re-run is idempotent: mike overwrites the version, and `LATEST_VERSION.txt` is rewritten.
- **Update checks from betas.** `LATEST_VERSION.txt` tracks stable only, so beta users are offered the next stable release, never other betas (planner).
- **The site repo's root.** mike's commits touch only `docs/`. The `LATEST_VERSION.txt` commit touches only that file.

**Assumptions:**
- The user has created `opennucleon/opennucleon.github.io`, enabled Pages from `gh-pages`, added a deploy key with write access, and stored its private half as the OpenNucleon secret `SITE_DEPLOY_KEY`.

## Phase 392.1: Publish Workflow

- [ ] M392.P1.T1 — Write the docs publish workflow
  - files: `.github/workflows/docs.yml`, `docs/user/mkdocs.yml` (the `extra.version.provider: mike` picker)
  - approach:
    - Trigger on `release: published` and `workflow_dispatch` with a `tag` input.
    - Check out the tag, download the release's `Nucleon-*-x86_64.AppImage` asset with `gh release download`, and extract it.
    - Run `tools/docs/build.sh` with `--binary` pointing at the extracted `NucleonRenderer`, inside a mike deploy:
      - `mike deploy --deploy-prefix docs --remote site --branch gh-pages --push dev` for a prerelease;
      - `mike deploy … X.Y latest --update-aliases` then `mike set-default … latest` for a stable release.
    - The `site` remote is the site repo over SSH, with `SITE_DEPLOY_KEY` loaded by `webfactory/ssh-agent` (or an equivalent pinned action).
    - Before the first stable release, `mike set-default dev`.
    - Set concurrency per the edge cases.
  - verify: `actionlint` passes. Dispatching on the newest beta tag publishes `https://opennucleon.github.io/docs/dev/`. Its node, Python and getting-started pages load, and `https://opennucleon.github.io/docs/` redirects to `dev`. The site repo's `gh-pages` commit touches only `docs/`.
  - size: M

- [ ] M392.P1.T2 — Write LATEST_VERSION.txt on stable releases
  - files: `.github/workflows/docs.yml`, `tools/release/latest-version.sh` (new)
  - approach:
    - `latest-version.sh <tag>` prints the four-line file in the format the app parses: `File version: 1`, `Software version: X.Y.Z`, `Development status: Release`, `Build number: 0`.
    - On a stable tag only, the workflow commits that file to the site repo's `gh-pages` root (a sparse checkout of `LATEST_VERSION.txt` alone, then push with the deploy key).
    - Add a unit check: a `bats`-free shell test or a small Python test under `tools/release/tests/` that feeds the output to the same parsing rules as `AppInstance.cpp:451-495`, restated in the test with a pointer to that code.
  - verify: The test passes for `v1.0.0` and `v1.2.3`, and rejects `v1.0.0-beta3`. Dispatching against a scratch stable-looking tag on a fork, or `act` locally, is not required. Instead, run the workflow step in dry-run mode (`DRY_RUN=1` prints the commit it would make) against `v1.0.0`, and check that the output matches the format. The real write is proven at M396 - Release 1.0 Cut.
  - size: M

**Verification gate:**
- `actionlint` passes.
- A dispatched run on the newest beta publishes `/docs/dev/`, with node, Python and getting-started pages reachable and the version picker shown.
- `latest-version.sh`'s test passes, and the dry run prints the expected file for `v1.0.0`.
- The site repo's `gh-pages` history shows the docs commits touching only `docs/`.

## Decisions

- 2026-11-10 — Docs publish to `opennucleon.github.io/docs/` with mike: `latest` and a frozen `<major>.<minor>` per stable release, and `dev` from betas (user).
- 2026-11-10 — The docs are built from the released AppImage rather than a fresh build: the reference then documents exactly the binary users download, and nothing compiles twice (planner).
- 2026-11-10 — `LATEST_VERSION.txt` tracks stable releases only, so a beta build is never offered another beta (planner).
- 2026-11-10 — Old branches: the orphan `docs` branch and this repo's 2014 `gh-pages` branch are deleted after 1.0, as a post-release step at M396 - Release 1.0 Cut, with archive tags created first (user).
