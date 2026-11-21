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

- [ ] M493.P1.T1 — Install ccache's HTTPS storage helper in the dev image
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

- [ ] M493.P1.T2 — Opt local builds into the remote cache via `.local/ccache-remote`
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

## Phase 493.2: CI remote cache

Precondition: the user has set the repo secret `CCACHE_REMOTE_STORAGE` to the public HTTPS URL.

- [ ] M493.P2.T1 — Use the remote cache in `remote-build.yml`, with a switch
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

- [ ] M493.P2.T2 — Measure CI wall time with and without the remote cache
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
