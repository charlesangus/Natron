# M493 - Shared Ccache Remote Storage

Every build keeps its own ccache: the main checkout's `.ccache`, a few worktrees' own `.ccache` dirs, and per-branch, per-preset `actions/cache` entries in `ci.yml`, `remote-build.yml` and `release.yml` (capped by GitHub's 10 GB quota). None of them share. This milestone adds a shared ccache remote storage layer behind all of them, so local builds, worktrees and CI runs reuse each other's objects. The local cache stays the first layer everywhere; ccache only asks the remote on a local miss.

The server already runs outside this repo, on the user's own infrastructure. It is an HTTP cache server that speaks ccache's `layout=bazel`, has an LRU size cap and requires basic auth. It has two kinds of endpoint:

- a public HTTPS URL, for GitHub Actions and anywhere else
- a plain-HTTP URL by docker service name, for containers attached to the server's docker network

The user keeps the real URLs, credentials and the network name privately. **Never write a hostname, IP, port, username, password or credentials path into the plan, the repo, a PR or a log.** Committed files and docs use placeholders such as `https://USER:PASS@cache.example.com|layout=bazel`.

What has been checked on the server: a compile in `natron-dev` (ccache 4.13.6) with a fresh local cache got a remote hit over the plain-HTTP docker-network URL. A 300 MB blob round-tripped over the public HTTPS URL.

## Scope

**In:**
- The HTTPS storage helper baked into the dev image.
- An opt-in remote cache for `devshell.sh` / `build.sh`, read from gitignored `.local/ccache-remote`.
- Remote storage in `remote-build.yml`. Then, if measurement shows it doesn't slow CI down, in `ci.yml` and `release.yml` too.
- README docs, with placeholders only.

**Out:**
- Any change to the cache server or its credentials. Both live outside this repo.
- Read-only credentials.
- Dropping `actions/cache`.

**Edge cases:**
- No `.local/ccache-remote` → builds stay local-only, exactly as today.
- Fork PRs get no secrets, so `CCACHE_REMOTE_STORAGE` expands to an empty string. ccache treats an empty value as no remote and runs local-only. This is the cache-poisoning guard: only same-repo runs can write.
- Server down or slow → ccache marks the backend failed for that compile and carries on locally. The build must never fail because of the cache.
- An existing `natron-dev` container needs no `--recreate`: the env var rides every `docker exec`, and the network is attached live.

**Assumptions:**
- The HTTPS helper works end to end against this server. Not yet run.
- Cross-host keys match. They should, because `build.sh` sets `CCACHE_BASEDIR` to the checkout root and `CCACHE_NOHASHDIR=1`, and every host uses the same dev image.
- The server's uplink can serve CI misses. A cold debug build is ~2 GB of objects.

## Phase 493.1: Local remote cache

- [x] M493.P1.T1 — Install ccache's HTTPS storage helper in the dev image
  - files: `tools/ci/local/Dockerfile`
  - approach:
    - In the "Additions on top of the base image" block, download `ccache-storage-http-go` v0.10.1 (`https://github.com/ccache/ccache-storage-http-go/releases/download/v0.10.1/ccache-storage-http-go-0.10.1-linux-amd64.tar.gz`).
    - Check it with `sha256sum -c` against `679a8337cdc32a8bc670a2a91a2c11624d174b41881f0fa14d3c6bd06addf3fe`.
    - Install the binary (`ccache-storage-http` in the tarball) as `/usr/local/bin/ccache-storage-https`.
    - Do NOT also install it as `ccache-storage-http`: that would replace ccache's built-in HTTP client, which is already verified for the docker-network path.
    - Pin the version and hash as `ARG`s next to the other pins, so `dev-image-tag.sh`'s content hash changes and CI pushes a new tag.
    - Update the comment saying nothing needs installing.
  - verify:
    - `tools/ci/local/devshell.sh --recreate` builds the image, then `devshell.sh which ccache-storage-https` prints the path.
    - Inside the container, compile a small `.cpp` twice, each time with its own empty `CCACHE_DIR`, with `CCACHE_REMOTE_STORAGE` set to the public HTTPS URL (from the user's private config, never a committed file). The second compile shows `remote_storage_hit 1` in `ccache --print-stats`.
    - `tools/ci/local/check-image-pins.sh` passes.
  - size: M

- [x] M493.P1.T2 — Opt local builds into the remote cache via `.local/ccache-remote`
  - files: `tools/ci/local/devshell.sh`, `tools/ci/local/build.sh`, `tools/ci/local/README.md`
  - approach:
    - Read the file in `devshell.sh`, resolved from the main checkout root via the git common dir (as `build.sh` resolves `CCACHE_BASEDIR`), so worktrees share one file. Only `KEY=value` lines count; blank and `#` lines are skipped.
    - `url=` is the ccache remote storage URL, e.g. `https://USER:PASS@cache.example.com|layout=bazel`.
    - Optional `network=` names a docker network. When the container isn't attached to it, `docker network connect` it after the container is up. The default `bridge` network can't reach host-published LAN ports on the user's build host, so the local path is `network=<server's docker network>` with the plain-HTTP service-name URL.
    - Pass `-e CCACHE_REMOTE_STORAGE=<url>` on every `docker exec`, beside the existing `CCACHE_MAXSIZE` pass-through, so a running container picks it up without `--recreate`.
    - Never echo the URL: it carries the password.
    - No file → no `-e`, no network change.
    - `build.sh` prints whether a remote cache is configured (yes/no only) in its banner, and its closing `ccache -s` already shows remote hits.
    - README: a "Remote cache" section covering the file format with placeholder values, the two URL kinds, and the off switch (delete the file).
  - verify:
    - Run a fresh `.worktrees/<id>/` debug build of a commit the main tree has built with the remote configured, after `ccache -z`. Use a scratch `CCACHE_DIR` holding only its own empty local cache, e.g. temporarily point `CCACHE_DIR_HOST` at a new dir. `ccache -s` should show most compiles as remote hits.
    - With the file removed, a build runs local-only and `ccache -p` shows `remote_storage` empty.
    - `bash -n` and shellcheck clean.
  - size: M

- [x] M493.P1.T3 — Reuse published image layers when building the dev image
  - files: `.github/workflows/dev-image.yml`, `tools/ci/local/devshell.sh`, `tools/ci/local/README.md`
  - approach:
    - Added at the user's request (2026-11-21): a Dockerfile change made the local fallback build recompile Cycles and MoonRay from scratch, because pulled images carry no build-cache metadata.
    - `dev-image.yml`: build with BuildKit, `--build-arg BUILDKIT_INLINE_CACHE=1`, and `--cache-from ghcr.io/charlesangus/natron-dev:latest`, so pushed images carry inline cache and unchanged layers are reused.
    - `devshell.sh`: the local fallback `docker build` uses the same inline-cache arg and `--cache-from` the `:latest` image; a missing or uncached `:latest` must not fail the build.
    - README: one line on why late Dockerfile layers rebuild cheaply.
  - verify: `bash -n` and shellcheck clean on devshell.sh; actionlint clean on dev-image.yml; after `main` publishes once, a local build of a late-layer change reuses the Cycles layer (post-merge check).
  - size: M

## Phase 493.2: CI remote cache

Precondition: the user has set the repo secret `CCACHE_REMOTE_STORAGE` to the public HTTPS URL.

- [x] M493.P2.T1 — Use the remote cache in `remote-build.yml`, with a switch
  - files: `.github/workflows/remote-build.yml`, `tools/ci/remote/build.sh`, `tools/ci/remote/README.md`
  - approach:
    - Add a `workflow_dispatch` boolean input `remote_ccache` (default `true`) and a matching `--no-remote-ccache` flag in `tools/ci/remote/build.sh`.
    - In the build job's `env:`, set `CCACHE_REMOTE_STORAGE` to `${{ (inputs.remote_ccache || github.event_name == 'push') && secrets.CCACHE_REMOTE_STORAGE || '' }}`.
    - Also set `CCACHE_COMPILERCHECK: content`, matching devshell, so compiler mtimes can't split keys between hosts.
    - Keep the `actions/cache` restore/save steps unchanged as the first layer.
    - The existing `ccache stats` step should show remote hits. Switch it to `ccache -s -v` if plain `-s` doesn't.
  - verify:
    - A dispatched `fast` run on a branch, made after a local `fast` build of the same commit, shows `Remote storage` hits in the `ccache stats` step.
    - A run with `remote_ccache=false` shows none.
    - The secret value appears masked in the logs.
    - `lint-ci` green.
  - size: M

- [x] M493.P2.T2 — Measure CI wall time with and without the remote cache
  - files: none (a measurement; results go in this file's Decisions section)
  - approach:
    - On one commit, dispatch `remote-build.yml` `fast` and `debug` twice each: once with `remote_ccache=false` and once with it on.
    - Clear the branch's `actions/cache` entries before each run (`gh cache delete`), so both start from the same local state: the main fallback key only.
    - Record each build step's duration, ccache local/remote hit counts, and whether remote fetches hit timeouts.
    - Remote is a win if it is no slower on any run and faster when the local layer misses.
  - verify: the four runs' numbers and the verdict are recorded in this file's Decisions section.
  - size: M

- [ ] M493.P2.T3 — Enable the remote cache in `ci.yml` and `release.yml`
  - files: `.github/workflows/ci.yml`, `.github/workflows/release.yml`
  - approach:
    - Do this only if M493.P2.T2's recorded verdict is a win. Otherwise strike this task through and record why.
    - `ci.yml`: set `CCACHE_REMOTE_STORAGE: ${{ secrets.CCACHE_REMOTE_STORAGE }}` and `CCACHE_COMPILERCHECK: content` in the build job's `env:`. Fork PRs get an empty value and stay local-only.
    - `release.yml` runs in `aswf/ci-vfxall` directly, not the dev image. Add a step that installs the same pinned helper (same version, same sha256 check) into `/usr/local/bin/ccache-storage-https` before the build, or switch the job to the dev image if that is a clean swap.
    - Keep `actions/cache` as the first layer in both.
  - verify:
    - A same-repo PR's `build-and-test` shows remote hits and passes.
    - A dispatched dry release build (or the next tag) shows remote hits.
    - `lint-ci` green.
  - size: M

**Verification gate:**
- A fresh worktree build served by remote hits (M493.P1.T2's check).
- `remote-build.yml` showing remote hits with the switch on and none with it off.
- The CI timing comparison recorded in Decisions.
- No hostname, IP, port, username or password in the diff or the logs.
- `format`, `lint-ci` and `build-and-test` green.

## Decisions
- 2026-11-20 — **Server stood up outside the repo (user):** an HTTP cache server with an LRU size cap. It was chosen over Redis (memory-bound) and plain WebDAV (no eviction). Its endpoints and credentials stay out of the public plan and repo.
- 2026-11-20 — **One read-write credential, no read-only one:** fork PRs never see secrets, so the only untrusted writer is already excluded.
- 2026-11-20 — **ccache's built-in client is HTTP-only;** HTTPS goes through the `ccache-storage-https` helper, hence M493.P1.T1.
- 2026-11-21 — Phase 493.1 code in d36f5d9c6:
  - **Image:** the Dockerfile pins and checksum-verifies the 0.10.1 HTTPS storage helper.
  - **Opt-in:** devshell.sh reads `url=` and `network=` from the main checkout's `.local/ccache-remote`, attaches the network live, and passes `CCACHE_REMOTE_STORAGE` by name only, so the password stays out of `ps`.
  - **Docs:** a README section with placeholders only.

  Unverified: the end-to-end HTTPS hit, and whether the layout option is written `layout=bazel` or `@layout=bazel`. **Needs the user:** create `.local/ccache-remote`, then a devshell `--recreate` to test. Phase 493.2 needs the repo secret `CCACHE_REMOTE_STORAGE`.
- 2026-11-21 — M493.P1.T3 code in 85476bc56 (milestone/m493-shared-ccache-remote-storage): BuildKit inline cache plus `--cache-from :latest` in dev-image.yml and the devshell.sh fallback. bash -n, shellcheck and actionlint clean. Layer reuse takes effect only after main publishes an image with inline cache, so it is a post-merge check.
- 2026-11-21 — **M493.P1.T1 verified:** in the recreated natron-dev (image ada6a1188a9c9f28), two compiles of a fresh file, each with its own empty CCACHE_DIR, over the public HTTPS URL: the first wrote 2 objects remotely, the second got `remote_storage_hit 1`, with no errors or timeouts. check-image-pins.sh passes. **The HTTPS helper needs `@layout=bazel`** (a helper custom attribute); `layout=bazel` is for the built-in http:// client. README fixed in d84d936ea. The user has added the repo secret `CCACHE_REMOTE_STORAGE`, and `.local/ccache-remote` now holds an HTTPS `url=`.
- 2026-11-21 — M493.P2.T1 code in ab912a864: `remote_ccache` dispatch input (default on; push runs on), `--no-remote-ccache` in tools/ci/remote/build.sh, `CCACHE_COMPILERCHECK: content`; actionlint and shellcheck clean. Remote-off baselines (fast, debug) dispatched on the branch with no actions/cache entries; remote-on runs follow once the local M493.P1.T2 test has filled the server, after deleting the branch caches the baselines save.
- 2026-11-21 — **M493.P1.T2 verified** on ab912a864's sources in .worktrees/m493 (natron-dev from image ada6a1188a9c9f28, HTTPS remote). Run A, debug, empty local cache: 1779 misses, 3549 remote writes, 0 errors/timeouts, 2 h 18 min at -j3. Run B, fresh build dir, its own empty local cache: **1777/1779 remote hits**, 0 errors, 9 min 27 s. With `.local/ccache-remote` moved away, devshell passes no `CCACHE_REMOTE_STORAGE` and `ccache -p` shows `remote_storage` empty. bash -n and shellcheck clean.
- 2026-11-21 — **P2.T2 first pass:** fast off 1996 s / on 1669 s (server had no fast objects, 0/1028 hits: no overhead); debug off 5039 s / on 5317 s with **0/1784 remote hits** although the server held every debug object from P1.T2. The consultant ruled out paths (base_dir relativises worktree and CI builds identically), image, compiler and flags; the store differed. The secret had been an http URL with the helper's `@` attribute.
- 2026-11-21 — **Fixes:** 2f1770c1a adds tools/ci/ccache-remote-layout.sh, used by CI and devshell, which sets `@layout=bazel` (https) or `layout=bazel` (http) whatever the stored spelling, and `ccache -z` after the CI restore. The user then re-entered the secret as the whole local line (`url=…`); ccache rejected it as an invalid scheme, **failed every compile**, and printed the URL with only the credentials masked. The logs of runs 44965552172 and 44965207715 were deleted; earlier M493 runs never mention the domain. 0d1709e24 accepts a copied `url=`/`docker_url=` key, turns the remote off with a fixed message for anything not http(s), masks the bare host, and checks `ccache --print-config` under the value. Verification debug run dispatched with the current secret.
- 2026-11-22 — 820ebc4d8 replaces the `ccache --print-config` check (not a ccache option; it rejected every value) with a one-file probe compile, checked in the M493 image: a malformed value fails it, an unreachable server passes.
- 2026-11-22 — **P2.T1 verified and P2.T2 verdict: win.** Remote-build debug, local layer cold (branch caches deleted): remote off 5039 s build step; **remote on 328 s, 1777/1784 remote hits** (run 44979152185, secret normalised to https, no domain in the log). Fast with a cold remote (0/1028 hits) 1669 s vs off 1996 s, so misses and uploads cost nothing measurable. No remote timeouts or errors. P2.T3 proceeds.
