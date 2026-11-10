# M393 - Landing Site

Build the public landing page at `https://opennucleon.github.io/` with Astro, in the `opennucleon/opennucleon.github.io` repo. It has a hero with the Nucleon logo and a one-line pitch, real screenshots, a feature overview, and a download section: the latest stable AppImage and tarball with checksums, plus a link to the newest beta. The site repo's own workflow builds it into the root of its `gh-pages` branch and leaves `docs/` and `LATEST_VERSION.txt` to M392 - Docs Publishing.

Context:
- **This milestone's code lives in another repo** (user, 2026-11-10: landing source in the site repo).
  - The PM clones `opennucleon/opennucleon.github.io` to `.worktrees/site/` and adds `/.worktrees/` to `.git/info/exclude` if it isn't there already.
  - It branches `milestone/m393-landing-site` there, and opens and merges the milestone PR against that repo's `main`.
  - Plan commits name the site repo's SHA and branch: `(code: <sha> on opennucleon.github.io:milestone/m393-landing-site)`.
  - No OpenNucleon PR comes from this milestone.
- **Runs after F17 - OpenNucleon Flag Day** (Deps M382 - Nucleon Logo And Artwork, a Feature member), so the art and the app name are final. The art is in OpenNucleon's `Gui/Resources/Branding/`, and its `README.md` gives the palette and each file's role.
- **Screenshots:** `tools/docs/screenshots/capture.sh <AppImage> <outdir>` in OpenNucleon, with the `landing-*` scenes from M388 - Docs Site Scaffold. Run it against the current release AppImage and copy the PNGs into the site repo.
- **`gh-pages` is shared** (`DECISIONS/2026-11-10-nucleon-1-0-docs-and-site.md`). This workflow owns the root and must preserve `docs/` and `LATEST_VERSION.txt`.
- **Minimum screen** is 1920x1080 at scale 1. The page must also read well on a phone.

## Scope

**In** (user, 2026-11-10: hero and screenshots, feature overview, download):
- An Astro project in the site repo with static output.
- **Hero:** the logo, the wordmark, a one-line pitch and a primary download button.
- **Screenshots:** the five `landing-*` captures, optimised by Astro's image pipeline.
- **Feature overview:** short sections on native GPU nodes, USD/3D with Hydra delegates, deep compositing, Python scripting, OCIO/ACES colour management and Linux-native packaging. Each section has one screenshot or none.
- **Download:**
  - the latest stable AppImage and tarball, with size and sha256;
  - system requirements;
  - a small "nightly beta" link to the newest prerelease;
  - links to the docs (`/docs/`) and to GitHub.
- A Pages deploy workflow in the site repo.

**Out:**
- A fork-of-Natron credit or community section on the landing page. The user left it out (2026-11-10); the credit lives in the README and the About dialog (M386 - Nucleon Branding And Packaging).
- The docs site and `LATEST_VERSION.txt`: M392 - Docs Publishing.
- New screenshot scenes in OpenNucleon: M388 - Docs Site Scaffold.
- Analytics and cookies.

**Edge cases:**
- **No stable release yet.** Before v1.0.0, the download section shows the newest beta as the primary button, labelled "Beta".
- **GitHub API unavailable or rate-limited.** Download data is fetched in the browser from the public Releases API. On failure the section falls back to a plain "All releases on GitHub" link, with no broken buttons. The page is still fully static and useful without JavaScript.
- **Asset matching.** Find assets by pattern (`Nucleon-*-x86_64.AppImage`, `Nucleon-*-linux-x86_64.tar.*`), never by a hardcoded version. Checksums come from the release's `SHA256SUMS` asset if one exists. Otherwise the checksum line is omitted, and P2.T1 notes whether `release.yml` should publish one.
- **Deploying beside the docs.** The deploy step must keep `docs/` and `LATEST_VERSION.txt` (`keep_files`, or a deploy that only replaces root-level site files).

**Assumptions:**
- The site repo exists and the PM's `gh` login can push branches and merge PRs there.
- Storm renders the `landing-3d` scene under Xvfb on llvmpipe well enough for a hero image. If it doesn't, capture that one scene on the user's GPU host.

## Phase 393.1: Site

- [ ] M393.P1.T1 — Scaffold the Astro site and its Pages deploy
  - files: (site repo) `package.json`, `astro.config.mjs`, `src/layouts/Base.astro`, `src/pages/index.astro`, `public/`, `.github/workflows/deploy.yml`, `README.md`
  - approach:
    - Use Astro with static output, no UI framework, and a single page plus a 404 page.
    - Theme colours come from the Branding README's palette.
    - `deploy.yml` builds on `main` pushes and on manual dispatch, and publishes `dist/` to `gh-pages` with `peaceiris/actions-gh-pages` (`keep_files: true`), or an equivalent that never deletes `docs/` or `LATEST_VERSION.txt`. Pin it.
    - Copy the mark and wordmark SVGs from OpenNucleon's `Gui/Resources/Branding/`.
  - verify: `npm run build` succeeds. After merging, `https://opennucleon.github.io/` serves the scaffold, and `https://opennucleon.github.io/docs/` and `/LATEST_VERSION.txt` still resolve if M392 - Docs Publishing has published them (otherwise, `gh-pages` still has any pre-existing `docs/` tree).
  - size: M

- [ ] M393.P1.T2 — Write the hero and feature sections with screenshots
  - files: (site repo) `src/components/{Hero,Features,Screens}.astro`, `src/assets/screens/*.png`
  - approach:
    - Run `capture.sh` against the current release AppImage for the `landing-*` scenes, and copy the PNGs in.
    - Agents draft the pitch and the feature copy (user, 2026-11-10: agents draft, user approves), from the board's `done` milestones and the project decisions: what is true in the shipped app, with no roadmap claims.
    - Keep each feature to a heading and two or three sentences.
  - verify: `npm run build` succeeds. Lighthouse (`npx lighthouse` against `astro preview`, or `unlighthouse`) scores at least 90 for performance and accessibility on desktop and mobile. Full-page screenshots at 1920x1080 and at 390x844 are sent to the user one file each, along with each `landing-*` capture, for approval.
  - size: M

- [ ] M393.P1.T3 — Build the download section
  - files: (site repo) `src/components/Download.astro`, `src/scripts/releases.ts`
  - approach:
    - A static section lists the system requirements and an "All releases on GitHub" link.
    - A small inline script fetches `https://api.github.com/repos/opennucleon/OpenNucleon/releases?per_page=20`, picks the newest non-prerelease and the newest prerelease, and fills in the buttons, sizes and checksums, following the edge cases.
    - No framework, and under 3 KB of script.
  - verify: In `astro preview` with a headless browser, against the live API, the beta-only state (before 1.0) shows the beta as primary. With the API blocked (route the request to fail), only the fallback link shows and there are no console errors. A captured fixture of the API response that includes a non-prerelease renders the stable button with its checksum, tested by serving the fixture locally.
  - size: M

**Verification gate:**
- The site repo's `npm run build` and deploy succeed, and `https://opennucleon.github.io/` serves the page.
- `/docs/` and `/LATEST_VERSION.txt` are untouched by the deploy.
- Lighthouse scores at least 90 for performance and accessibility on desktop and mobile.
- The download section works in all three states (beta only, stable, API failure).
- The user has approved the page from the full-page screenshots and each landing capture, sent one file each.

## Decisions

- 2026-11-10 — The landing page is built with Astro, and its source lives in the `opennucleon.github.io` repo rather than in OpenNucleon (user).
- 2026-11-10 — Downloads: the latest stable AppImage and tarball as the primary button, plus a small nightly-beta link (user).
- 2026-11-10 — The landing page has no fork-credit or community section (user picked hero, features and download only).
- 2026-11-10 — Download data is fetched in the browser from the Releases API, with a static fallback, so a new release shows without rebuilding the site and needs no cross-repo token (planner).
- 2026-11-10 — The landing screenshot scenes live in OpenNucleon's harness (M388 - Docs Site Scaffold), so this milestone ships one PR in one repo (planner).
