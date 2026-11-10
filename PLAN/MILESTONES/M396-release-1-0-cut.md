# M396 - Release 1.0 Cut

Cut Nucleon 1.0. Write the release checklist, re-capture every public screenshot with the renamed app, tag and publish `v1.0.0-rc1`, and support the user's UAT on their AMD and NVIDIA hosts. Fix and re-tag RCs until the user approves, then tag `v1.0.0` and verify that every public surface updated: the release page, the docs `1.0` and `latest`, `LATEST_VERSION.txt`, the landing download button and the in-app update prompt. Finish with the post-release steps.

**Held:** not eligible for pickup until the user names which Features and milestones must land before 1.0 (see `# Open questions`). Those are then added to this row's `Deps`.

Context:
- **Everything it ships:**
  - M391 - In-App Docs Bundle (offline help);
  - M392 - Docs Publishing (`docs.yml`, `LATEST_VERSION.txt`);
  - M393 - Landing Site;
  - M394 - Release Versioning (tags, `tag-info.sh`, pruning);
  - M395 - Release Notes 1.0 (highlights, changelog);
  - the F17 - OpenNucleon Flag Day rename.
- **UAT practice:**
  - UAT AppImages go in `build/appimages/` with a same-named steps `.md` that includes a "Round 2+" section.
  - Screenshots go to the user one file each.
  - Approvals given against an older build still merge, with post-merge checks listed on the board.
- **Old branches** (user, 2026-11-10): the orphan `docs` branch and the 2014 `gh-pages` branch are deleted after 1.0, with archive tags created first. The user runs the deletion.

## Scope

**In:**
- `docs/RELEASING.md`, the release checklist for RCs, stable releases and patch releases.
- `build/appimages/Nucleon-1.0.0-rc<N>-x86_64.md`, the UAT steps.
- Re-capturing the docs and landing screenshots on the RC.
- The RC loop.
- Tagging `v1.0.0`.
- Post-release verification.
- The version bump on `main` to 1.1.0.
- Archive tags for the old branches.

**Out:**
- Feature work. Anything the UAT finds that isn't a release blocker becomes a new milestone through `/cat-plan` and is listed under Known issues.
- Deleting the old branches, which the user does after the archive tags exist.

**Edge cases:**
- **A blocker found in UAT.** Fix it on a branch from `main` as a task added to this milestone (small) or a new milestone (large), merge it, and tag `rc<N+1>`. Each RC repeats the UAT round's "Round 2+" steps only.
- **`main` moves during the RC.** RCs and the final tag are cut from the commit the user approved, and later commits wait for 1.1. If a blocker fix is needed, it goes on `main` and the next RC is cut from `main`'s head after the gates pass.
- **A post-release surface fails** (for example, the docs deploy). Re-dispatch the failed workflow with the tag. The release itself is not redone.

**Assumptions:**
- Unconfirmed: the user's hosts for UAT are the AMD dev host and the NVIDIA GPU host.

## Phase 396.1: Prepare

- [ ] M396.P1.T1 — Write the release checklist
  - files: `docs/RELEASING.md`
  - approach: Cover the steps in order:
    - preconditions (green `main`, must-land list done);
    - regenerate the 1.0 changelog and finalise Known issues;
    - re-capture the screenshots;
    - tag an RC with `gh workflow run release.yml -f tag=v1.0.0-rc<N>`;
    - the UAT round;
    - tag the stable release;
    - verify the public surfaces;
    - bump `project(VERSION)` to the next minor;
    - the post-release steps (archive tags, then the user deletes the old branches);
    - for patch releases, branch from the tag as `release/X.Y` and apply the same steps.
  - verify: Each command in the file is checked with `--help` or a dry run. The rendered file is linked in the PR body.
  - size: M

- [ ] M396.P1.T2 — Re-capture the screenshots on the renamed app
  - files: `docs/user/content/img/**` (OpenNucleon), `src/assets/screens/*` (site repo, through a site-repo PR)
  - approach: Run `tools/docs/screenshots/capture.sh` against the newest beta AppImage for every scene. Replace the docs PNGs in OpenNucleon and the `landing-*` PNGs in the site repo, each through its own PR. The site repo PR follows M393 - Landing Site's cross-repo practice.
  - verify: No screenshot shows "Natron" (check the title bar, splash and About text by eye). Every new PNG is sent to the user one file each for approval. Both PRs merge, and the docs `dev` and the landing page show the new images.
  - size: M

- [ ] M396.P1.T3 — Finalise the 1.0 notes
  - files: `docs/user/content/releases/1.0.md` (Known issues), `docs/user/content/releases/1.0-changelog.md`
  - approach: Regenerate the changelog to `main`'s head with `tools/release/changelog.py`. Update Known issues from the open `blocked` rows and any non-blocking UAT findings.
  - verify: `tools/docs/build.sh` passes `--strict`. The user approves the updated Known issues.
  - size: M

## Phase 396.2: Release Candidate

- [ ] M396.P2.T1 — Tag rc1 and run the UAT round
  - files: `build/appimages/Nucleon-1.0.0-rc1-x86_64.md` (local, not committed)
  - approach:
    - Dispatch `release.yml` for `v1.0.0-rc1` from the approved commit, and download the AppImage to `build/appimages/`.
    - Write the UAT steps:
      - fresh-HOME install and run of the AppImage and the tarball;
      - the getting-started First Comp walkthrough;
      - Help → Documentation and a node "?" button offline (network disabled);
      - the Python reference page;
      - the About dialog;
      - a 3D/USD scene in the viewer;
      - a GPU node;
      - project save and load (`.nucz`);
      - `NucleonRenderer` on that project;
      - the update check, enabled, which shows no prompt while no stable release exists.
    - Ask the user to run it on both hosts.
  - verify: The release shows as a prerelease titled "Nucleon v1.0.0-rc1". `--version` reports 1.0.0 RC 1. The docs `dev` updated. The user's UAT outcome is recorded in `## Decisions`, with any blockers added as tasks per the edge cases.
  - size: M

## Phase 396.3: Release

- [ ] M396.P3.T1 — Tag v1.0.0 and verify the public surfaces
  - files: none (the release workflow does the tagging)
  - approach: Once the user approves an RC, dispatch `release.yml` for `v1.0.0` on that same commit.
  - verify: Check each surface:
    - The release is not a prerelease, is titled "Nucleon v1.0.0", and has the highlights and the changelog link in its body.
    - `https://opennucleon.github.io/docs/` redirects to `latest`, which is `1.0`.
    - `https://opennucleon.github.io/LATEST_VERSION.txt` reads 1.0.0, Release.
    - The landing page's primary button is the 1.0.0 AppImage, with its checksum.
    - An `rc1` AppImage with the update check enabled offers 1.0.0 (an Xvfb screenshot of the dialog is sent to the user).
  - size: M

- [ ] M396.P3.T2 — Post-release steps
  - files: `CMakeLists.txt` (`project(VERSION 1.1.0)`)
  - approach:
    - Bump the version to 1.1.0 on `main` through a PR.
    - Create and push the archive tags `archive/natron-docs` (at `origin/docs`) and `archive/natron-gh-pages` (at `origin/gh-pages`).
    - Report to the user the two branch deletions to run (`git push origin --delete docs gh-pages`). The user runs them.
  - verify: The next `main` beta is tagged `v1.1.0-beta1`. Both archive tags exist on the remote at the old branch heads.
  - size: S

**Verification gate:**
- The user has approved an RC from the UAT round.
- `v1.0.0` is published and every public surface in P3.T1 checks out.
- `main` reports 1.1.0, and its next beta is `v1.1.0-beta1`.
- The archive tags exist, and the user has been told about the branch deletions.

## Decisions

- 2026-11-10 — 1.0 goes through an RC tag and the user's UAT checklist on their hosts before `v1.0.0` is tagged (user).
- 2026-11-10 — Which Features and milestones must land before 1.0 is the user's call, to be made later. Until then this milestone is held (user).
