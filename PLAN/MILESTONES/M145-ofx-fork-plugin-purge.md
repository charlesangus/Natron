# M145 - OFX Fork Plugin Purge

Once every port in this round has landed, delete the OFX plug-ins they replaced from our forks and remove the host-side skip mechanism M120 - Native Node Port Kit built. After this, a retired plug-in leaves no trace in the shipped bundles or in the host. That is the clean break the skip list was standing in for.

Deps: every milestone that adds `Engine/OfxRetired/<ofx-id>` markers, which are M113 - Native Warp Nodes, M121 - Native Routing And Mix, M122 - Native Colour Utilities, M123 - Native Sharpen And Glow, M124 - Native Time Remaps, M125 - Native Multi-Frame Nodes, M127 - Native OCIO Nodes, M128 - Native Shape Generators, M129 - Clean-Sheet Noise Node, M130 - Native Edge-Preserving And Directional Filters, M132 - Native Lens Distortion and M140 - Native Metadata Utilities. It also depends on M143 - Bundled Plugin Bug Fixes, which bumps the same fork refs. M131 - Native Defocus Family retires nothing.

Context:
- The forks are `charlesangus/openfx-misc` (`OPENFX_MISC_REF`, `Misc.ofx` and `CImg.ofx`) and `charlesangus/openfx-io` (`OPENFX_IO_REF`, the OCIO plug-ins in `IO.ofx`), pinned in `tools/ci/local/fetch-assets.sh`. M140 - Native Metadata Utilities already drops the openfx-metadata build from the asset fetch.
- The markers in `Engine/OfxRetired/` are the authoritative list of what to delete. Each marker's file name is an OFX plug-in ID.

## Scope

**In:**
- Deleting every plug-in that has a marker from the openfx-misc and openfx-io fork builds.
- Bumping both refs.
- Removing `Engine/OfxRetired/`, its generator, the `loadOFXPlugins()` skip and the test hook.
- Updating the docs and tests that describe the mechanism.

**Out:**
- Retiring plug-ins that have no native replacement yet: none, since they stay OFX.
- Upstreaming the deletions: none, because the forks are ours.

**Edge cases:**
- A marker ID that is shared code inside a plug-in that stays OFX (for example a helper used by several CImg filters): delete only the registration and the plug-in's own sources, and keep the shared code.
- A port milestone that was cancelled instead of landing: its plug-ins were never marked, so they stay.

**Assumptions:**
- Each fork plug-in is registered through its bundle's plug-in list (`ofxsMain`/`getPluginIDs` sources), so it can be dropped by removing its factory and sources.

## Phase 145.1: Fork deletions

- [ ] M145.P1.T1 — Delete retired plug-ins from the openfx-misc fork
  - files: the openfx-misc fork: each marked plug-in's source directory, the bundle plug-in lists (`Misc/Misc.cpp`, `CImg/CImg.cpp` or their equivalents) and the Makefiles or CMake lists
  - approach: list `Engine/OfxRetired/` on main, keep the IDs that the openfx-misc bundles provide, and remove each one's factory registration and its now-unused sources in one fork commit. Keep shared helpers that remaining plug-ins still use. Push to a branch of the fork.
  - verify: the fork builds, and its plug-in list, dumped by loading `Misc.ofx` and `CImg.ofx` into Natron and listing `getPluginIDs()`, contains none of the marked IDs and still contains every unmarked one.
  - size: M

- [ ] M145.P1.T2 — Delete retired OCIO plug-ins from the openfx-io fork
  - files: the openfx-io fork: `OCIO/` plug-in sources and the `IO.ofx` plug-in list and build lists
  - approach: same as P1.T1, for the `fr.inria.openfx.OCIO*` markers. The readers and writers in `IO.ofx` stay.
  - verify: the fork builds, and `IO.ofx`'s plug-in list has no OCIO IDs while its readers and writers remain.
  - size: M

## Phase 145.2: Host cleanup

- [ ] M145.P2.T1 — Bump the fork refs
  - files: `tools/ci/local/fetch-assets.sh`
  - approach: point `OPENFX_MISC_REF` and `OPENFX_IO_REF` at the P1 commits once they are merged into the forks' main branches. Update the comment above each ref with a one-line summary of the deletions.
  - verify: at the phase build, a fresh `fetch-assets.sh` run fetches the new refs, and the full ctest passes with the markers still present.
  - size: S

- [ ] M145.P2.T2 — Remove the retired-ID skip mechanism
  - files: `Engine/OfxRetired/` (deleted), `Engine/OfxHost.cpp`, `Engine/OfxHost.h`, `Engine/CMakeLists.txt`, `Tests/OfxRetired_Test.cpp` (deleted)
  - approach:
    - Delete the markers and their README, the CMake generator and the skip set, the skip in `loadOFXPlugins()` and the test hook.
    - Delete `OfxRetired_Test`.
    - In `NativePluginList_Test`, drop the marker-consistency checks but keep `RetiredOfxIdsResolveToASingleNativeVersion`, which now holds because the bundles no longer contain the IDs.
  - verify: at the phase build, the full ctest passes and `git grep -n OfxRetired` finds nothing outside the plan.
  - size: M

- [ ] M145.P2.T3 — Drop the retirement step from the port checklist
  - files: `Engine/Nodes/README.md`
  - approach: remove the marker line from the "Adding a native node" checklist. A future port deletes its plug-in from the fork directly and bumps the ref, since lanes are no longer running in parallel for this round.
  - verify: the checklist has no `OfxRetired` reference and names the fork deletion step instead.
  - size: S

**Verification gate:**
- Full build and ctest pass on a clean build dir with freshly fetched assets.
- `python3 tools/ci/smoke_test.py` passes against the AppImage.
- A dump of every loaded plug-in ID contains none of the formerly marked IDs, and every native replacement is registered.
