# M426 - UDIM Textures In Delegates

UDIM live textures from M311 - UDIM Live Textures render in Cycles and MoonRay, not only
Storm. Split out of M311's Phase 311.4 (user, 2026-11-13) so F11 - Texture Painting does not
wait on F9 - Delegate Projection; this milestone waits for F9 to land instead (its `Deps` name
M343 - Live Textures In Delegates, an F9 member).

## Scope

**In:**
- Cycles and MoonRay show every declared UDIM tile of a live texture.
- A missing tile renders magenta with a warning, as in Storm.

**Out:**
- UDIM in Storm, the store and the resolver — M311 - UDIM Live Textures (done).
- Texture painting across tiles — F11 - Texture Painting.

## Phase 426.1: Delegates

- [ ] M426.P1.T1 — UDIM live textures in Cycles
  - files: `Engine/Nodes/Scene/LiveTextureSceneIndex.cpp`, `Engine/Nodes/Scene/LiveTextureSharedMemory.cpp`, `Tests/Scene/LiveTexture_Test.cpp`, a Cycles golden under `Tests/fixtures/native-golden/USDRender/`
  - approach: the shared-memory path contains `<UDIM>`, one file per tile. hdCycles never fills `ImageTextureNode::tiles`, so the scene-index filter sets a `tiles` int array from the declared tiles; `ImageManager::add_image` then substitutes `%04d`. Keep the Storm path unchanged.
  - verify: a two-tile Cycles golden shows both tiles; removing one tile renders it magenta and logs the warning; the existing Storm UDIM golden still matches.
  - size: L
- [ ] M426.P1.T2 — UDIM live textures in MoonRay
  - files: `Engine/Nodes/Scene/LiveTextureSceneIndex.cpp`, `Engine/Nodes/Scene/LiveTextureSharedMemory.cpp`, `Tests/Scene/LiveTexture_Test.cpp`, a MoonRay golden under `Tests/fixtures/native-golden/USDRender/`
  - approach: MoonRay globs `prefix*` to find tiles, so each live texture's shared-memory prefix must be unique to its tile set (no other texture's files may match the glob). Make the prefix unique in `LiveTextureSharedMemory` without changing what Storm and Cycles resolve.
  - verify: a two-tile MoonRay golden shows both tiles; two UDIM live textures in one scene don't pick up each other's tiles; the Cycles and Storm UDIM goldens still match.
  - size: M

**Verification gate:** the LiveTexture gtests and the Storm, Cycles and MoonRay UDIM goldens pass in a remote `build.sh release --tests --gui "Tests/gui/scene_live_texture_udim.py"`; screenshots of both delegates' two-tile renders go to the user for sign-off.
