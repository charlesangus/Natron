## Pending

### 2026-11-05T12:50:00-05:00 — change-request
- refs: M249, M250, M251, M252
- **Lobby UAT report: `.local/Collected_World_Lobby/World_Lobby.usd` crashes Cycles and renders wrong in both delegates.** Please add tasks to these milestones, as they fit, for findings 1 to 5. The user wants these fixed. Sibling entry below covers the island scene and shares finding 1.

  **Tested:** `Natron-render-delegates-m249-m254.AppImage` (stack tip #188, M253 - Delegate Render Settings) in natron-dev under Xvfb and llvmpipe, through USDRead, USDRenderSettings, USDRender and Write, camera `/World/Cameras/CamLobbyWide`, under gdb. The bundle is stripped, so frames inside hdCycles have no names. The progressive viewer with Cycles or MoonRay was not exercised; it uses the same delegates.

  **Not the cause:** composition (2,786 prims, opens in under a second) and Storm (draws in 10 s at 4.4 GB; its only complaints are the 16-light truncation and the dome light with no texture).

  **1. hdCycles segfaults on any material with no universal surface output (crash, M249 - Bundle Cycles Delegate).**
  - Four Lobby materials have only `outputs:mdl:surface`: `Looks/Stucco`, `Looks/Steel_Carbon`, `Looks/Adobe_Octagon_Dots`, `Looks/Marble_Tile_12`. Log: `Could not get a material network for /World/assembly_BaseBuildingL0/Looks/Stucco`, then SIGSEGV under `Hd_PrimTypeIndex<HdSprim>::SyncPrims`.
  - Likely cause, from reading v5.2.0 source, not symbols: `HdCyclesMaterial::Sync` (`src/hydra/material.cpp:258`) logs the error but still calls `_shader->tag_update()`, which dereferences `graph->output()` (`src/scene/shader.cpp:357`) on a shader that never got a graph.
  - Repro in 30 lines: a sphere bound to a Material whose only output is `outputs:mdl:surface.connect` to a Shader with `info:implementationSource = "sourceAsset"` and `info:mdl:sourceAsset = @OmniPBR.mdl@`. Same crash offsets. MoonRay renders the same file. With the four materials deactivated, the Lobby renders through Cycles in 16 s.
  - Fix in the fork (`charlesangus/cycles`, `natron/deep-5.2`): give such a shader a default graph so the mesh renders with a fallback, keep the message, and bump `NATRON_CYCLES_COMMIT`. Add the small scene as a ctest fixture. It also ought to reach the node as a warning naming the material (M251 - Cycles Rendering).

  **2. Cycles' UsdPreviewSurface translation is wrong for normal maps and emission (wrong image, M251 - Cycles Rendering).**
  - `UsdUVTexture` `scale` and `bias` have no mapping (358 warnings each), so a normal map's 0..1 values are never remapped to -1..1, and `UsdPreviewSurface.normal` goes straight to the principled normal. Evidence: with the 92 `normal` connections cut and nothing else changed, the black vase, reception desk and wall sculpture light correctly (`out/lobby2-Cycles/x.02.png` against `out/lobby3-Cycles/x0.02.png`). Speckle on the walls drops but does not go away at 32 samples; not separated from plain noise.
  - `emissiveColor` warns `Could not find parameter` on all 151 surfaces: the mapping names `emission`, which the node does not have as a socket, so emission looks lost.
  - `opacityThreshold`, `useSpecularWorkflow`, `specularColor`, `occlusion`, `displacement`, `clearcoat` and `fallback` are also dropped. Together this is about 2,000 warnings per render, which is most of the "bunch of errors".
  - Fix in the fork: apply scale and bias, route `normal` through a normal-map node, fix the emission socket, then decide which of the rest to implement and silence the others.

  **3. MoonRay rejects every texture that is not mip-tiled (wrong image, M252 - MoonRay Rendering).**
  - All 1,256 Lobby images are plain jpg or png. 331 unique files fail with `Invalid image file ...: image was untiled`, then `FATAL: failed to open texture file` and `unknown oiio sample failure`. The interior renders black. The window view blows out from the dome light. 60,659 pixels are NaN.
  - Repro: one textured quad averages 0.012 with the `.jpg` and 0.12 with the same image through `maketx`. `libtexturing_sampler.so` carries `accept_untiled`, `autotile` and `automip` strings; the exact setting was not read from source.
  - Fix: allow untiled reads through a `build-moonray.sh` patch, or convert to a cache on demand. Plain jpg and png are normal USD assets, so asking users to run `maketx` is not a fix.
  - The node warning `MoonRay could not render these prims as authored` names 115 meshes when the fault is their textures. It should name the texture files.
  - **Open:** with every image mip-tiled the texture errors reach zero, but a 4-sample 1080p frame did not finish in 560 s, against 41 s when the textures failed. The cause was not separated (slow, hung, or a side effect of my test copy, which stored mip-tiled TIFFs under `.jpg` and `.png` names). Expect another step here.

  **4. MoonRay aborts the whole process on a frame range of this scene (crash, M250 - Bundle MoonRay Delegate, not root-caused).**
  - USDRender frames 1 to 6 wrote only frames 1 and 3, then `terminate called without an active exception` under `arras4::client::Client::createLocal`, from `hdMoonray::ArrasRenderer::connect`, from `endUpdate`. A single frame works. Eight frames of the small `scene-cycles.usda` fixture work.
  - That message usually means a joinable `std::thread` was destroyed or assigned over. `LocalSession::start` and its members are the place to look. Parallel frame renders are one suspect, since 1 and 3 were written without 2. Needs a symbol build. The delegate runs in Natron's process, so this loses the user's session.

  **5. hdMoonray posts Coding Errors per textured material (M252 - MoonRay Rendering).** Two `Attempted to get value of type 'TfToken' from empty VtValue` and one `MarkAllRprimsDirty ... requires emulation` for each. Also seen on a one-material scene. Not traced. If the dirty call really does nothing without emulation, material edits may not re-sync geometry.

  **6. For the user to decide, not a bug: camera exposure.** All cameras checked author `exposure = -10`. No renderer path in Natron reads it, and the Cycles frame averages about 8,000 (8,000 / 1024 is about 8). Worth raising at UAT whether USDRender and the viewer should apply it, since this will read as "everything is blown out".

  Logs, layers and scripts are in `.local/usd-triage/` on the host with pm-id `gpu` (`README` not written; `render_scene.py`, `run_in_container.sh`, `mini/`, `out/`).

### 2026-11-05T12:50:30-05:00 — change-request
- refs: M249, M254
- **Island UAT report: viewing `.local/island/usd/island.usda` in a USDRead kills Natron.** Please add tasks for items 1 and 2. M249 - Bundle Cycles Delegate owns the image and M254 - Progressive Viewer Render touches the viewport; move them elsewhere if they fit better, since the bug itself predates both. Same setup as the Lobby entry above.

  **Crash:** `terminate called after throwing an instance of 'std::bad_alloc'` about 30 s after connecting the viewer. Reproduced three times, once with no memory cap.

  **Root cause: a 32-bit overflow in OpenUSD 26.08, not running out of memory.**
  - `HdStPtexMipmapTextureLoader::generateBuffers` (`pxr/imaging/hdSt/ptexMipmapTextureLoader.cpp:1051-1053`) does `int pageStride = ...; new unsigned char[pageStride * numPages]`. A Ptex that packs past 2 GiB goes negative.
  - An `operator new` shim caught the request: `size=18446744071637779632`, last Ptex opened on that thread `textures/isDunesB/Color/dune0001_geo.ptx`, which works out to about 2.07 GiB packed.
  - At the throw, RSS was 18.5 GB on a 62 GB host, and about 30 GB was still free in the uncapped run.
  - Larger files exist (`isBeach/Color/beach_geo.ptx` 2.4 GB, `isDunesB/Color/dune0002_geo.ptx` 1.7 GB on disk), so expect several to overflow.

  **Why it kills the process:** the exception crosses TBB in `HdSt_TextureObjectRegistry::Commit`, rethrows on the GUI thread and goes up through `UsdImagingGLEngine::Render` from the paint event. `Gui/SceneViewport.cpp:2394` has no handler, so it terminates. Any exception inside Hydra would do the same.

  **1. Stop the overflow.** Two routes, and neither was tried:
  - Set a Ptex budget with `HdStResourceRegistry::SetMemoryRequestForTextureType`. `optimizePacking` honours `targetMemory`, which should keep each file under the overflow and also bound RAM. No USD rebuild, if Natron can reach the registry through the engine. Preferred, because of item 3.
  - Patch USD to `size_t`. USD comes from the base image, so this means rebuilding it.

  **2. Catch exceptions around `engine->Render` in the viewport** and report through `natronSceneError` instead of terminating, discarding the engine since Hydra's state is then unknown. Check the `HydraRenderImpl.cpp` render calls (lines 1218, 1235, 2891) as well; USDRender runs them on a pool thread.

  **3. Open: the scene is very heavy once it stops crashing.**
  - With `isBeach` and `isDunesB` deactivated there is no crash, but RSS plateaus at 41 GB and it had not drawn after more than 10 minutes, with the main thread pegged in llvmpipe's JIT code and the GUI blocked. Software GL exaggerates this, and llvmpipe keeps textures in process memory.
  - Real-GPU behaviour is **not tested**, because this host had no display. The scene has 13,313 Catmull-Clark meshes, 2.9 M PointInstancer instances and 26,659 Ptex references (3,644 files). Needs a run on RADV before calling it fixed.

  **Delegates on this scene:**
  - Cycles hits finding 1 of the Lobby entry: no material has a universal surface output, only `outputs:glslfx:surface`, `outputs:ri:surface` and `outputs:ri:displacement`. Once that crash is fixed, all 13,471 materials will fall back unless Cycles is pointed at a usable network, and Ptex would still need support. Treat proper island looks in Cycles or MoonRay as new scope for the user to decide.
  - MoonRay produced no frame in 270 s; not investigated.
