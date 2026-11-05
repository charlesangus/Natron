## Pending

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
