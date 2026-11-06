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
- Large image: size is the input's RoD, with no cap (no hand-holding size caps). Out of shared memory gives the fallback and a warning naming the node.

**Assumptions:**
- Cycles and `execComp` both load an OIIO that honours plug-in search paths, built from the same pinned version. T1 proves it.
- M233.P2.T1's pre-render collects URIs whichever delegate is active. T1 confirms it; a miss means a fix in T2.

## Phase 343.1: Route

- [ ] M343.P1.T1 — Prove the shared OIIO route on both renderers
  - files: scratch only; findings recorded in this file's `## Decisions`
  - approach: read how OIIO picks a format for a name like `natron://node/rgba?v=hash` (`ImageInput::create`, its extension dispatch, and `<format>.imageio.so` search paths), and whether M233's `LiveTextureResolver::_GetExtension` value means the URI must change. Build a throwaway plug-in that serves a synthetic gradient, then render a textured quad with Cycles and with MoonRay through `execComp`. Confirm tiled-and-mipped presentation passes MoonRay's `accept_untiled=false`, and that Cycles' tx-cache step leaves the name alone. If either fails, compare the alternatives: a Cycles `ImageLoader` subclass in the charlesangus fork, a MoonRay patch via M325 - MoonRay Fork, or running MoonRay's in-process `hd_moonray_debug` renderer. A cache of files on disk needs the user's approval: M233 promises no disk round trip.
  - verify: both renderers show the gradient at the expected values; the chosen route and its URI form are recorded with source citations; any alternative needing approval is put to the user before P2 starts.
  - size: L

## Phase 343.2: Shared store and plug-in

- [ ] M343.P2.T1 — Serve live textures from shared memory
  - files: `Engine/Nodes/Scene/LiveTextureStore.{h,cpp}`, `Tests/Scene/LiveTexture_Test.cpp`
  - approach: store rendered images in session-named, read-only shared-memory objects that keep M233's immutable, hash-keyed entries and LRU budget, so Storm's resolver and the plug-in read the same pixels with no copy. Build the mip chain on first export to a delegate. Remove stale objects from dead sessions at startup.
  - verify: gtest — another process maps an entry and reads the expected pixels; eviction while mapped leaves the mapping readable; a stale object from a dead session is removed; the existing `LiveTexture_Test` cases still pass.
  - size: L

- [ ] M343.P2.T2 — The OIIO format plug-in
  - files: new `Engine/Nodes/Scene/LiveTextureOiio/` plug-in source, `Engine/CMakeLists.txt`, `tools/ci/local/stage-bundle.sh`, `Tests/Scene/LiveTextureOiio_Test.cpp`
  - approach: an `ImageInput` serving T1's URI form from P2.T1's store: float RGBA, linear, tiled, with the mip chain. It depends on no Natron library, so it loads in `execComp`. Install it on the OIIO plug-in search path in the build tree and the AppImage, and set that path in `MoonRayRuntime`'s environment and for Cycles.
  - verify: gtest — `OIIO::TextureSystem` with `accept_untiled=false` samples an entry to its expected values at each mip; a missing entry fails to open without crashing; the bundle's startup check finds the plug-in.
  - size: L

## Phase 343.3: Delegates

- [ ] M343.P3.T1 — Cycles renders live textures
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`, one reference crop (≤256 px, half-float, zip)
  - approach: make sure `natron://` reaches `filename` unchanged and that the pre-render covers it. If Hydra hands Cycles an empty resolved path, fix it in the scene-index layer, as the implicit-surface filter does. Report a failed open as the node warning. Include normal-map and opacity inputs, subject to M251 - Cycles Rendering's normal-map fix.
  - verify: gtest — a Constant into a card's material renders its colour through Cycles; an animated Ramp differs at frames 1 and 10; a deleted source gives the fallback and a warning naming it.
  - size: M

- [ ] M343.P3.T2 — MoonRay renders live textures
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/MoonRayRuntime.{h,cpp}`, `Tests/Scene/USDRenderMoonRay_Test.cpp`, one reference crop (≤256 px, half-float, zip)
  - approach: pass the session's store name and the plug-in path to `execComp` through the session environment. hdMoonray skips a `UsdUVTexture` whose resolved path is empty (`Material.cc:277`), so the resolver must return the URI there. Replace the misleading "could not render these prims" notice with the failing source node's name.
  - verify: gtest — the same three cases as P3.T1 pass through MoonRay in `execComp`, and a second frame range in one process doesn't leak shared-memory objects.
  - size: M

- [ ] M343.P3.T3 — Upstream edits restart progressive refinement
  - files: `Gui/SceneViewport.cpp`, `Tests/gui/scene_live_texture_delegates.py`
  - approach: in M254 - Progressive Viewer Render, an upstream edit changes the URI's hash, so the delegate must re-sync the material and restart. Make sure both delegates reload the image rather than reusing a cached name. Fix any stale-image case in the viewer's sync.
  - verify: an Xvfb GUI script at 1920x1080 selects each delegate, edits a Constant feeding a card, and shows the new colour in the viewer; screenshots go to the user (one file each).
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDRenderCycles_Test`, `USDRenderMoonRay_Test` and `LiveTexture*_Test` pass; `scene_live_texture_delegates.py` passes under Xvfb; the user OKs the screenshots and tests a UAT AppImage with a plate sequence on a card in both delegates.

## Decisions

- 2026-11-05 — **Filed outside the materials discussion (user):** "we definitely need to support live natron materials", regardless of the MaterialX outcome.
- 2026-11-05 — **Planned route: one OIIO plug-in over shared memory (cat-plan, proven by M343.P1.T1):** an in-process store can't reach `execComp`, and hdCycles and hdMoonray have no shared Hydra-side image path, but both open images through OIIO.
- 2026-11-05 — **Projection goes in its own milestone (user: "absolutely"; split by cat-plan):** M344 - Camera Projection In Delegates builds on this one.
