# M343 - Live Textures In Delegates

Make M233 - Live Node Textures' `natron://` images render in Cycles and MoonRay, live and animated, as they do in Storm, in both USDRender and the progressive viewer. Filed on the user's instruction (2026-11-05), separately from the MaterialX and bespoke-shader discussion. Neither delegate can use M233's path today. hdCycles never calls `ArResolver` or `HioImage`: `HdCyclesMaterial` maps `file` to an image node's `filename`, which `OIIOImageLoader` opens (`src/hydra/material.cpp:176`, `src/scene/image_oiio.cpp`). hdMoonray passes the path to the `UsdUVTexture` DSO, which samples through `OIIO::TextureSystem` (`dso/map/UsdUVTexture/UsdUVTexture.cc`, `lib/rendering/texturing/sampler/TextureSampler.cc`), and that sampling runs in `execComp`, a separate process (`Engine/Nodes/Scene/MoonRayRuntime.h:46` on the M250 - Bundle MoonRay Delegate branch), so M233's in-process `LiveTextureStore` is out of its reach. Both delegates do share one image library, so the planned route is a single OIIO format plug-in over a store the render process can map.

## Scope

**In:**
- `natron://` textures authored by `USDMaterial`'s image inputs render in Cycles and MoonRay: diffuse, roughness, metallic, normal, emissive and M233's alpha-to-opacity path.
- Animated sources follow the frame in USDRender. Editing an upstream node restarts M254 - Progressive Viewer Render's refinement.
- One mechanism for both delegates, and any other OIIO-based delegate later: an OIIO image format plug-in that reads rendered images from read-only shared memory. It presents them as tiled and mip-mapped, because MoonRay's sampler rejects anything else.
- A live texture that can't be served gives a node warning naming the source node, and the render continues with the `UsdUVTexture` fallback.

**Out:**
- MaterialX `image` nodes: none, MaterialX is set aside (user, 2026-11-05).
- Native shader nodes' texture inputs: M353 - Cycles Native Shaders and M354 - MoonRay Native Shaders, which reuse this route.
- USDProject3D in Cycles and MoonRay: M344 - Camera Projection In Delegates.
- `<UDIM>` live textures in delegates: M311 - UDIM Live Textures extends this once both have landed.
- Natron Utility: M281 - Utility Materials And Lights reads `natron://` through `HioImage`.
- MoonRay's rejection of untiled files on disk: M252 - MoonRay Rendering (inbox Lobby report, finding 3). This milestone serves only live images.
- GPU-resident hand-off: none (M233 v1 is CPU too).

**Edge cases:**
- Source deleted, or its render failed: fallback plus the node warning, as in Storm.
- Store entry evicted while the render process is reading: a mapping stays valid until unmapped, so the read completes. A new open of an evicted entry takes the fallback path.
- Natron crashes with images exported: named shared-memory objects would outlive it, so startup removes stale entries from dead sessions.
- Two Natron sessions on one host: object names carry the session, so they can't serve each other's images.
- Static input (same hash every frame): one URI, loaded once per render session.
- Large image: size is the input's RoD, with no cap (no hand-holding size caps). Out of shared memory keeps the previous image; the render warns naming the node and aborts (user decision, 2026-11-07, below).

**Assumptions:**
- Cycles and `execComp` both load an OIIO that honours plug-in search paths, built from the same pinned version. T1 proves it.
- M233.P2.T1's pre-render collects URIs whichever delegate is active. T1 confirms it; a miss means a fix in T2.

## Phase 343.1: Route

- [x] M343.P1.T1 — Prove the shared OIIO route on both renderers
  - files: scratch only; findings recorded in this file's `## Decisions`
  - approach: read how OIIO picks a format for a name like `natron://node/rgba?v=hash` (`ImageInput::create`, its extension dispatch, and `<format>.imageio.so` search paths), and whether M233's `LiveTextureResolver::_GetExtension` value means the URI must change. Build a throwaway plug-in that serves a synthetic gradient, then render a textured quad with Cycles and with MoonRay through `execComp`. Confirm tiled-and-mipped presentation passes MoonRay's `accept_untiled=false`, and that Cycles' tx-cache step leaves the name alone. If either fails, compare the alternatives: a Cycles `ImageLoader` subclass in the charlesangus fork, a MoonRay patch via M325 - MoonRay Fork, or running MoonRay's in-process `hd_moonray_debug` renderer. A cache of files on disk needs the user's approval: M233 promises no disk round trip.
  - verify: both renderers show the gradient at the expected values; the chosen route and its URI form are recorded with source citations; any alternative needing approval is put to the user before P2 starts.
  - size: L

## Phase 343.2: Shared store and plug-in

- [x] M343.P2.T1 — Serve live textures from shared memory
  - files: `Engine/Nodes/Scene/LiveTextureStore.{h,cpp}`, `Tests/Scene/LiveTexture_Test.cpp`
  - approach: store rendered images in session-named, read-only shared-memory objects that keep M233's immutable, hash-keyed entries and LRU budget, so Storm's resolver and the plug-in read the same pixels with no copy. Build the mip chain on first export to a delegate. Remove stale objects from dead sessions at startup.
  - verify: gtest — another process maps an entry and reads the expected pixels; eviction while mapped leaves the mapping readable; a stale object from a dead session is removed; the existing `LiveTexture_Test` cases still pass.
  - size: L

- [x] M343.P2.T2 — The OIIO format plug-in
  - files: new `Engine/Nodes/Scene/LiveTextureOiio/` plug-in source, `Engine/CMakeLists.txt`, `tools/ci/local/stage-bundle.sh`, `Tests/Scene/LiveTextureOiio_Test.cpp`
  - approach: an `ImageInput` serving T1's URI form from P2.T1's store: float RGBA, linear, tiled, with the mip chain. It depends on no Natron library, so it loads in `execComp`. Install it on the OIIO plug-in search path in the build tree and the AppImage, and set that path in `MoonRayRuntime`'s environment and for Cycles.
  - verify: gtest — `OIIO::TextureSystem` with `accept_untiled=false` samples an entry to its expected values at each mip; a missing entry fails to open without crashing; the bundle's startup check finds the plug-in.
  - size: L

## Phase 343.3: Delegates

- [x] M343.P3.T1 — Cycles renders live textures
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`, one reference crop (≤256 px, half-float, zip)
  - approach: make sure `natron://` reaches `filename` unchanged and that the pre-render covers it. If Hydra hands Cycles an empty resolved path, fix it in the scene-index layer, as the implicit-surface filter does. Report a failed open as the node warning. Include normal-map and opacity inputs, subject to M251 - Cycles Rendering's normal-map fix.
  - verify: gtest — a Constant into a card's material renders its colour through Cycles; an animated Ramp differs at frames 1 and 10; a deleted source gives the fallback and a warning naming it.
  - size: M

- [x] M343.P3.T2 — MoonRay renders live textures
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/MoonRayRuntime.{h,cpp}`, `Tests/Scene/USDRenderMoonRay_Test.cpp`, one reference crop (≤256 px, half-float, zip)
  - approach: pass the session's store name and the plug-in path to `execComp` through the session environment. hdMoonray skips a `UsdUVTexture` whose resolved path is empty (`Material.cc:277`), so the resolver must return the URI there. Replace the misleading "could not render these prims" notice with the failing source node's name.
  - verify: gtest — the same three cases as P3.T1 pass through MoonRay in `execComp`, and a second frame range in one process doesn't leak shared-memory objects.
  - size: M

- [x] M343.P3.T3 — Upstream edits restart progressive refinement
  - files: `Gui/SceneViewport.cpp`, `Tests/gui/scene_live_texture_delegates.py`
  - approach: in M254 - Progressive Viewer Render, an upstream edit changes the URI's hash, so the delegate must re-sync the material and restart. Make sure both delegates reload the image rather than reusing a cached name. Fix any stale-image case in the viewer's sync.
  - verify: an Xvfb GUI script at 1920x1080 selects each delegate, edits a Constant feeding a card, and shows the new colour in the viewer; screenshots go to the user (one file each).
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDRenderCycles_Test`, `USDRenderMoonRay_Test` and `LiveTexture*_Test` pass; `scene_live_texture_delegates.py` passes under Xvfb; the user OKs the screenshots and tests a UAT AppImage with a plate sequence on a card in both delegates.

## Decisions

- 2026-11-07 — The GUI crash on a viewer colour change is a Cycles fork bug: `HdCyclesMaterial::Sync` rebuilds the graph via `set_graph` without tagging the shader modified, so a freed image slot is sampled (fixed upstream in 260185984a). User: fix it in the charlesangus/cycles fork and bump the pin, not a build-time patch.

- 2026-11-07 — /dev/shm (user): CI passes a larger `--shm-size`. When /dev/shm is full, the store keeps its hold on the previous image, and the render warns and aborts rather than showing magenta.

- 2026-11-07 — USDWrite with a deleted texture source (user): warn, naming the material, and export the material's own colour; don't fail the export.

- 2026-11-07 — Deleted source implemented at 7c95b00cd: a hidden persistent mark per input set only by node deletion, so a deliberate disconnect clears the slot; USDWrite now fails an export with a deleted source (pending the user's call).

- 2026-11-07 — Deleted source (user): "missing textures also render magenta". A material whose texture source was deleted shows magenta too, not its own colour.

- 2026-11-07 — A missing live texture shows bright magenta, not black, so it's obvious (user); the warning stays.

- 2026-11-07 — **Route (user):** delegates read a real path, `/dev/shm/natron-<session>-<uri hash>.natron`, while USD keeps the `natron://` asset, so no fork patches or pin bumps. A scene-index filter registered for Cycles and MoonRay only sets the path and leaves Storm on its URI. A missing texture: the filter removes the texture node and feeds the `UsdUVTexture` fallback value downstream, keeping the existing warning. This replaces P3.T1's "`natron://` reaches `filename` unchanged" and P3.T2's passing the store name to `execComp`; only `OPENIMAGEIO_PLUGIN_PATH` is passed. `stage-bundle.sh` is in `tools/release/`. A small `/dev/shm` (64 MB in Docker) sends real plates to the fallback.

- 2026-11-05 — **Filed outside the materials discussion (user):** "we definitely need to support live natron materials", regardless of the MaterialX outcome.
- 2026-11-05 — **Planned route: one OIIO plug-in over shared memory (cat-plan, proven by M343.P1.T1):** an in-process store can't reach `execComp`, and hdCycles and hdMoonray have no shared Hydra-side image path, but both open images through OIIO.
- 2026-11-05 — **Projection goes in its own milestone (user: "absolutely"; split by cat-plan):** M344 - Camera Projection In Delegates builds on this one.
- 2026-11-07 — **Route proven, with one change: delegates get a real tmpfs path, not the `natron://` URI (M343.P1.T1, subject to the user's OK):** the shared OIIO plug-in works in both renderers unpatched, but only under a name that exists on the filesystem. Both check before they reach OIIO: Cycles `src/util/image_metadata.cpp:358` (`Filesystem::exists` in `oiio_load_metadata`; `natron/deep-5.2` at 3dbd87c1) and MoonRay `lib/rendering/texturing/sampler/TextureSampler.cc:93` (`std::filesystem::exists` in `getHandle`). With a `natron://` name the plug-in is never called: Cycles logs "does not exist" and shades magenta, MoonRay logs a fatal to `execComp`'s stderr and shades white, and neither fails the render.
  - **Form:** USD keeps M233's `natron://<node>/<layer>?v=<hash>` as the authored asset path, and `LiveTextureResolver::_GetExtension` is untouched. The delegate-facing name is `/dev/shm/natron-<session>-<hash of the whole URI>.natron`, with the whole URI hashed because a content hash alone repeats across nodes. It is tmpfs, the file `shm_open` would create, so M233's no-disk promise holds. No store name needs passing through the environment; only `OPENIMAGEIO_PLUGIN_PATH` does.
  - **Dispatch:** `.natron` selects the plug-in directly (`src/libOpenImageIO/imageioplugin.cpp:639,659`, OIIO 3.1.16). The extensionless URI opened only through the try-all-readers fallback (`:640-643`, `:724`), after a plug-in rescan on every create (`:660-665`).
  - **Evidence (`build/m343-t1/`):** a throwaway `natron.imageio.so` served a 256 px, 64 px-tiled, 9-level float RGBA gradient on the M252 - MoonRay Rendering bundle (`/tmp/m252-moonray/app2`), rendering a quad at 128 px through USDRead, USDRenderSettings, USDRender and Write. Compared with the same gradient as a `maketx` file over the central 80% in R and G, Cycles shows 0.0000 max error and MoonRay 0.0005 max (0.0000 mean), through both a directly authored path and a `natron://` asset whose resolved path is that file. The plug-in's log shows reads from `NatronRenderer.bin` (36 tiles) and from `execComp` (16 tiles). `execComp` inherits the variable through `arras4_core_impl/lib/execute/Environment.cc:51`. `TextureSystem` with `automip`, `autotile`, `accept_untiled` and `accept_unmipped` all off samples every level correctly.
  - **Cycles' tx cache leaves the name alone:** `auto_texture_cache` defaults off (`src/scene/scene.h:87`) and nothing in `src/hydra` sets it, so `image_oiio.cpp:62-64` clears the cache path and `get_filepath()` returns the original. Nothing was written beside the file. A tiled, mipped input takes Cycles' on-demand tile path (`src/scene/image.cpp:493`).
  - **Stale premise:** the shipped MoonRay already accepts untiled, unmipped images (`tools/ci/local/build-moonray.sh:166-183`). Tiles and mips stay because they avoid decoding whole images and take Cycles' tile path, and they pass with those checks on.
  - **Alternatives:** patching both `exists` checks to keep the URI needs two fork patches, a Cycles and a MoonRay pin bump, and an image rebuild, with a new patch to carry at each MoonRay update. A `Cycles ImageLoader` subclass solves only Cycles, and `hd_moonray_debug` changes MoonRay's process model. A disk cache is unneeded. None of these needs approval now.
  - **Where the path is set (recommended):** a scene-index filter registered for Cycles and MoonRay, as `CyclesAovs.cpp:614` does, rewrites the `file` parameter to `SdfAssetPath(uri, tmpfsPath)`, because both delegates read only `GetResolvedPath()` (hdCycles `src/hydra/node_util.cpp:75`, hdMoonray `ValueConverter.cc:232`). It leaves Storm on its URI and gives the "first export to a delegate" a trigger for the mip chain. Having `_Resolve` return the path instead also works and needs no registration, but it moves Storm onto `ArFilesystemAsset`, and the resolver cannot tell which delegate is asking.
  - **Fallback needs Natron's help:** neither delegate honours `UsdUVTexture.inputs:fallback` for a missing image. Cycles has no such socket (`src/hydra/material.cpp:296` warns) and shades magenta. hdMoonray drops the node when the resolved path is empty (`Material.cc:277`), so the surface takes its default 0.18, and a failed open shades white. Neither reaches Natron except as a log line. So the filter must act on a missing entry: delete the texture node and set the downstream input to the fallback value, which also works when shared memory is full. The warning stays on the consumer's `LiveTextureStore::getFailure` path (`USDMaterial.cpp:442`, `USDEmissiveMaterial.cpp:247`), and an export failure must be recorded there too.
  - **For M343.P2:** take `lock_guard` in `read_native_tile` and make `seek_subimage` a no-op at the current level; without them a multi-frame Cycles render corrupted the heap about 15 frames in (`malloc(): unaligned tcache chunk`), and with them a 60-frame run was clean. Use `posix_fallocate`, because a write past a full tmpfs after `ftruncate` is SIGBUS rather than an error. Docker's default `/dev/shm` is 64 MB (natron-dev and this sandbox), and one 2K float image with mips is about 44 MB, so gtests must stay small and the fallback path will fire in containers on real plates. Both renderers can close and reopen a file (`image_oiio.cpp:25` caps handles at 128; OIIO's `max_open_files`), so a reopen after eviction would fail mid-render: the plug-in should keep a path's mapping alive in the process past `close()`, or the store must keep entries a render session uses.
  - **Not proven here:** the bundle predates M233 in main, so a scratch `ArResolver` stood in for `LiveTextureResolver`, and the store, Storm and the viewer were not exercised. Non-power-of-two chains, alpha association (Cycles reads `oiio:UnassociatedAlpha`, `image_metadata.cpp:434`) and a `Cycles` build at the current pin are for P2.T2 and P3.T1. Pre-render: `USDRender.cpp:638` and `SceneViewport.cpp:4002` call `prepare()` with no delegate check, by reading only.
- 2026-11-10 — Merged into F9 as #244 (879e8c947). Cycles reads live textures raw (sourceColorSpace=raw) with channel-packed alpha; removed materials release their files at the next removal (PM call, pending user OK).
