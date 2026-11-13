# M325 - MoonRay Fork

Fork the MoonRay sources Natron builds (openmoonray and the submodules we patch: hydra-moonray, mcrt_dataio, scene_rdl2/arras as needed) under charlesangus, as was done for Cycles (charlesangus/cycles natron/deep), and move every patch `tools/ci/local/build-moonray.sh` applies with sed (IsSupported override, HostName debug print, depth channel name, session-definition log levels) into commits on a `natron/...` branch. The build script then pins the fork by commit and checksum and applies no patches (user, 2026-11-04: "fork moonray, similar to cycles, don't patch it in this repo").

## Scope

**In:**
- Fork the five repos `tools/ci/local/build-moonray.sh` edits under charlesangus, each with a `natron/v2026.29.1` branch: `openmoonray` (super-repo: `CMakeLists.txt` lines and `.gitmodules`), `arras4_core` (`Client.cc`), `moonray` (`TextureSampler.cc`), `mcrt_dataio` (`ClientReceiverFb.cc`), `hdMoonray` (`RendererPlugin.cc` ×2, `RenderBuffer.cc`, `Light.cc`, `RenderDelegate.cc`).
- One commit with its reason per edit the script makes today: the five `add_subdirectory` drops, `moonray_sdr_plugins`, the SessionNotification call, IsSupported, HostName, TextureSampler, the depth channel name, lightLink/MarkAllRprimsDirty, the disable-lighting null check, and the sessiondef log levels.
- hdMoonray overrides `GetMaterialRenderContexts` to return `{moonray}`, so it reads `outputs:moonray:surface` and falls back to universal outputs.
- `build-moonray.sh` pins the fork by commit, applies no source patches and writes a SOURCE file, like `build-cycles.sh`.

**Out:**
- Cycles' own sed lines in `build-cycles.sh`.
- Configure flags that are not source edits (`-include`s, `UDPSYSLOG_NO_BOOST`, the MaterialX alias include, the ISPC fetch).
- Forking unpatched submodules (`scene_rdl2`, `cmake_modules`, `mcrt_denoise`, `moonshine`, `mcrt_messages`, `mcrt_computation`).
- The MoonRay fixes M345 - VDB Rendering Fixes and M354 - MoonRay Native Shaders call for (they land in the fork in those milestones).

**Edge cases:**
- A GitHub fork of the super-repo still points its submodules at dreamworksanimation: the fork's `.gitmodules` names the charlesangus URLs and its gitlinks the new commits.
- GitHub archive tarballs drop submodules, so the pin is by commit (commit IDs already hash the content), with SOURCE listing every repo's URL and commit.
- If the sessiondefs are generated at install time, the template gets the edit.

**Assumptions:**
- The pinned upstream stays `d96c6e30` (v2026.29.1).
- The image's USD lacks Ndr, so `moonray_sdr_plugins` is dropped unconditionally.
- UsdImaging appends the universal render context after the delegate's contexts.

## Phase 325.1: Fork

- [x] M325.P1.T1 — Carry each submodule patch as a commit on the four submodule forks
  - files: forks `charlesangus/arras4_core`, `charlesangus/moonray`, `charlesangus/mcrt_dataio`, `charlesangus/hdMoonray`, branch `natron/v2026.29.1` from each gitlink commit at `d96c6e30`
  - approach: PM step first: `gh repo fork dreamworksanimation/<repo> --clone=false` for each. The agent clones into /tmp, finds where `hd_*.sessiondef` comes from, and writes one commit per sed block with the script's comment as the message; the PM pushes.
  - verify: applying the script's blocks to a clean upstream checkout and diffing against the fork tip shows nothing outside the commits; every "did not apply" grep in the script passes on the fork.
  - size: M

- [x] M325.P1.T2 — The super-repo fork points at the forks
  - files: `charlesangus/openmoonray` branch `natron/v2026.29.1`: `.gitmodules`, gitlinks, `CMakeLists.txt`, `arras/`, `moonray/`, `moonray/hydra/CMakeLists.txt`
  - approach: PM step: fork and push. One commit drops the dropped subdirectories; another repoints the four patched submodules' URLs and gitlinks at P1.T1's tips.
  - verify: `git ls-tree HEAD` gitlinks equal the P1.T1 tips; `.gitmodules` URLs resolve; no dropped `add_subdirectory` remains.
  - size: M

## Phase 325.2: Pin and render context

- [x] M325.P2.T1 — build-moonray.sh pins the fork and patches nothing
  - files: `tools/ci/local/build-moonray.sh`, `tools/ci/local/Dockerfile`
  - approach: replace the version-tag clone with `NATRON_MOONRAY_REPO`/`NATRON_MOONRAY_COMMIT`, fetched by SHA and checked. `fetch_submodule` reads the full `.gitmodules` URL, not just the basename, and fails on any host other than charlesangus or dreamworksanimation. Delete every sed block and the Python edit, keeping the post-install greps as checks. SOURCE lists the URL and commit of the super-repo and each submodule and says there are no build-time patches.
  - verify: `rg -n 'sed -i|python3 -' tools/ci/local/build-moonray.sh` shows no source edits; at the phase build the image builds and `MoonRayDelegate`, `USDRenderMoonRay` and `MoonRayAovs` pass with no test changes.
  - size: M

- [x] M325.P2.T2 — hdMoonray reads the `moonray` render context
  - files: fork `lib/hydramoonray/RenderDelegate.{h,cc}`, then the gitlink bump in the super fork and `NATRON_MOONRAY_COMMIT`; `Tests/Scene/USDRenderMoonRay_Test.cpp`
  - approach: override `GetMaterialRenderContexts() const` to return `{TfToken("moonray")}`. PM step: push it and bump the gitlink and pinned commit before the phase build. Test one material with both `outputs:moonray:surface` (a DwaBaseMaterial in one colour) and a universal UsdPreviewSurface (another colour), plus a universal-only material.
  - verify: gtest — the first material renders the moonray colour and the universal-only one its preview colour.
  - size: M

## Phase 325.3: Record

- [x] M325.P3.T1 — Docs follow the fork
  - files: `tools/ci/local/README.md`, the M345, M354 and M347 milestone notes on the override (plan-scoped: PM files an inbox note instead of editing other milestones)
  - approach: describe the forks, their branch and the pin; say no build-time patches remain.
  - verify: no doc in the repo mentions build-time MoonRay patches.
  - size: S

**Verification gate:** PR CI green. Changing `build-moonray.sh` changes the dev-image tag hash, so `dev-image.yml` (called from `ci.yml` and `remote-build.yml`) builds and pushes the new image; the release gate passes on it with the three MoonRay ctests unchanged and the new render-context test passing. The local natron-dev container is recreated from the new ghcr tag, not built locally.

## Decisions

- 2026-11-12 — **Elaborated (consultant, PM engineering calls):** fork the five patched repos only (the other six are fetched unpatched from dreamworksanimation); pin by commit, since archive tarballs drop submodules and commit IDs already hash content, with SOURCE listing each repo; branch `natron/v2026.29.1`, following Cycles' `natron/deep-5.2`. The patch set is ten edits, not the four the stub named. Forking under charlesangus follows the user's 2026-11-04 instruction ("fork moonray, similar to cycles").
- 2026-11-12 — **P1.T1 done, pushed:** `natron/v2026.29.1` on charlesangus/arras4_core 036936259 (base 17c49f1), moonray ffb02edaa (eef67ae), mcrt_dataio fe9dc6364 (7d52281), hdMoonray 5bcbe1d74 (986121d; five code commits plus the sessiondef templates). Sed output and the fork tips diff clean; the sessiondef log-level edit lives in `sessions/hd_{single,multi}.sessiondef.in`, which `configure_file` installs. The IsSupported edit is applied unconditionally (USD 26.08). The super-repo's `.gitmodules` URLs are relative, so P1.T2 must make the six unpatched submodules absolute dreamworksanimation URLs, or a fork of openmoonray would look for them under charlesangus.
- 2026-11-13 — P1.T2: `charlesangus/openmoonray` `natron/v2026.29.1` at 35379cc, on upstream v2026.29.1 d96c6e3, in three commits:
  - drops the six subdirectories the Natron build doesn't use;
  - points arras4_core, moonray, mcrt_dataio and hdMoonray at the forks (gitlinks 036936259, ffb02edaa, fe9dc6364, 5bcbe1d74) and makes the other 16 submodule URLs absolute upstream URLs;
  - drops `moonray/materialx_shaders`. Its repository isn't public upstream, so recursive init failed on it, and it only built under `BUILD_MATERIALX_SHADERS`.
- 2026-11-13 — Phases 2 and 3 done:
  - 52c110e64: pin by commit, no patches. All ten former patches are confirmed in the forks. The `moonray_sdr_plugins` drop and the IsSupported fix now apply unconditionally.
  - 02fcdb159: README.
  - hdMoonray 5f378826b reads the `moonray` render context; openmoonray 25f332c10 is the new pin.
  - M345/M347/M354's notes on the override go through the inbox.
  - Gate running; it also rebuilds the dev image.
- 2026-11-13 — Gate green (run 43430041370 on 52c110e64, dev image rebuilt). PR #272 into main opened; review round 1 by a Claude reviewer (Codex exhausted).
- 2026-11-13 — Review round 1 closed: README pin rule added (4790c2b6c). Declined in this PR, since any `build-moonray.sh` edit forces a dev-image rebuild: drop the dead `-DBUILD_MATERIALX_SHADERS=OFF`, fix the header's stale materialx_shaders reason (~:12), reword the SOURCE text so the unpatched upstream submodules don't read as forks (~:196), and delete or justify the `check_origin` comment (~:58). For the next milestone that edits the script.
