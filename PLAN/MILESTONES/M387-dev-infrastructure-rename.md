# M387 - Dev Infrastructure Rename

Rename the build and CI machinery to match the product. The dev image becomes `ghcr.io/opennucleon/nucleon-dev`, the container `nucleon-dev`, and the packaging and CI environment variables `NUCLEON_*`. The maintainer skill is `skills/nucleon-maintainer`. Every workflow and script that refers to these follows. The rename script renames most of the text. This milestone makes the infrastructure behind it real: publish the image under the new name, prove local and remote builds still work, and update the workflow permissions.

**Held:** starts only at the user's go-ahead, as part of F17 - OpenNucleon Flag Day.

Context:
- The image and container names appear in about 90 files on `main`: `tools/ci/local/*.sh`, `tools/ci/local/Dockerfile`, `tools/ci/remote/*`, `.github/workflows/{ci,remote-build,release,dev-image}.yml`, `tools/release/container/Dockerfile`, `tools/release/check-startup-container.sh` and `tools/bench/*`. M385 - OpenNucleon Code Rename's script has already rewritten the text. The image under the new name doesn't exist until P1.T1 publishes it.
- After the repo transfer, GHCR packages stay under `charlesangus`. The dev-image workflow must publish to `ghcr.io/opennucleon/nucleon-dev` with the org's package permissions.
- Remote builds pull the image by its content hash (`ghcr.io/…:<hash>`). Local builds use the running container by name.

## Scope

**In:**
- Publishing the dev image under the new name.
- A local devshell and build under the `nucleon-dev` container name.
- A remote build through `tools/ci/remote/build.sh` on the transferred repo.
- The release and nightly workflows producing `Nucleon-*` artifacts.
- `skills/nucleon-maintainer`.

**Out:**
- Deleting the old `charlesangus/natron-dev` package. That is the user's call after the Feature lands.
- Memory files and personal tooling outside the repo, which the user maintains.
- Re-recording GL goldens. The image contents don't change, only the name.

**Edge cases:**
- **Hosts with an old `natron-dev` container running.** The local scripts look for `nucleon-dev`. The PR body and `tools/ci/local/README` (or the script's error message) tell the user to stop the old container gracefully (`docker stop -t 30`, then `docker rm`) and start the new one with `devshell.sh`.
- **Worktree lanes** started before the switch carry the old scripts until they merge `main`.

**Assumptions:**
- The user has created the `opennucleon` org, transferred the repo, and granted the workflow package-write permission there.

## Phase 387.1: Image And Workflows

- [ ] M387.P1.T1 — Publish the dev image under the new name
  - files: `.github/workflows/dev-image.yml` (or whichever workflow builds the dev image), `tools/ci/local/Dockerfile`, `tools/ci/remote/` (image-name references)
  - approach: Check that `rename.py` set the image to `ghcr.io/opennucleon/nucleon-dev` in every place, with the same content-hash tag scheme. Dispatch the dev-image workflow on the milestone branch so the image publishes under the org. Fix the permission or login steps it needs.
  - verify: `docker pull ghcr.io/opennucleon/nucleon-dev:<hash>` succeeds for the hash `tools/ci/remote/build.sh` computes.
  - size: M

- [ ] M387.P1.T2 — Prove remote, release and nightly builds on the new names
  - files: `.github/workflows/{remote-build,release,ci}.yml`, `tools/ci/remote/build.sh`
  - approach: Run `tools/ci/remote/build.sh fast --tests` on the milestone branch. Dispatch the release workflow in dry-run or pre-release mode, if it has one, to check the artifact names. Fix every leftover old name or path. Check that `gh` calls in the scripts use the `opennucleon/OpenNucleon` repo slug.
  - verify: The remote build passes and produces `Nucleon-*.AppImage`. A release dry run, or the release workflow's packaging job, produces correctly named artifacts.
  - size: M

## Phase 387.2: Local Tooling

- [ ] M387.P2.T1 — Switch local devshell and build to the nucleon-dev container
  - files: `tools/ci/local/{devshell,build,test,package}.sh`, `tools/ci/local/syntax-check.py`, `tools/bench/` scripts that name the container
  - approach: Use `nucleon-dev` as the container name throughout. When the scripts find a running `natron-dev` container, they print a one-line instruction: `docker stop -t 30 natron-dev && docker rm natron-dev`. They never stop it themselves.
  - verify: With the old container stopped as instructed, `devshell.sh` brings up `nucleon-dev` from the new image, and `build.sh` with a `fast` local build succeeds.
  - size: M

- [ ] M387.P2.T2 — Rename the maintainer skill
  - files: `skills/nucleon-maintainer/` (renamed by the script), `skills/nucleon-maintainer/SKILL.md`
  - approach: Check the renamed skill's frontmatter `name:` and its text, which `rename.py` rewrote. Fix any instruction that the rename made wrong, such as paths to deleted art or old container names.
  - verify: `rename.py --check` passes. The skill's frontmatter `name` matches its directory.
  - size: M

**Verification gate:**
- The remote `fast` build with tests passes on the new image name.
- A local `nucleon-dev` devshell build succeeds.
- `rename.py --check` exits 0.
- `git grep -n 'natron-dev'` returns nothing outside `docs/decisions/`.
- The PR targets `feature/f17-opennucleon-flag-day`.
