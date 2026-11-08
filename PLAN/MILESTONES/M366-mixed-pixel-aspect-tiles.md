# M366 - Mixed Pixel Aspect Tiles

A Crop-style tile with source pixel aspect 1 and output pixel aspect 2 reads black outside `bounds ∩ RoI` unless a larger cache entry exists, so its result varies with cache contents. Reported under M300 - Unified Tiered Cache, and it predates it. The user's framing (2026-11-07): pixel aspect is display metadata and a pixel is still a pixel, so first ask why RoI or cache keys convert by pixel aspect at all, before patching. The fix may change the documented GPU contract.

## Scope

**In:**
- An audit of every pixel aspect conversion in RoI, RoD and cache-key code, with the reason each exists.
- A design note answering whether pixel aspect should touch pixel-space RoI and cache keys at all, approved by the user.
- The fix the note chooses, with the tile's result independent of cache contents, and the GPU contract doc updated to match.

**Out:**
- Viewer display of non-square pixels, which stays display metadata.
- Any change to format or pixel aspect metadata propagation beyond what the fix needs.

**Edge cases:**
- Deep renders, which convert input RoI by the input's pixel aspect (`EffectInstanceRenderDeep.cpp` :433).
- Nodes that support multiple clip pixel aspects, against the forced clip pixel aspect path (`EffectInstance.cpp` ~:8221-8338).
- Proxy and mipmap scales combined with a pixel aspect other than 1.

## Phase 366.1: Why convert at all

- [ ] M366.P1.T1 — Audit pixel aspect in RoI and cache keys
  - files: `Engine/EffectInstance.cpp` (`toPixelEnclosing`/`toCanonical` at ~942, 1118, 1519, 1691, 1742, 1800, 1914, 2347, 2872-2899, 3213, 4211, 8221-8338), `Engine/EffectInstanceRenderRoI.cpp` (:642, :781), `Engine/EffectInstanceRenderDeep.cpp`, `docs/cache-invalidation/image-caches.md`, `docs/cache-invalidation/keys.md`, a new `docs/pixel-aspect.md`
  - approach: on the default branch after M300 - Unified Tiered Cache lands, list each conversion with the pixel aspect it uses (node's or input's), what space it maps between, and whether removing it changes any pixel. Find the documented GPU contract on pixel aspect and quote it. Note the `_pixelAspect` key fields stored as a constant 1 in `image-caches.md`.
  - verify: the table is in `docs/pixel-aspect.md`, every listed site has a row, and the GPU contract text is quoted with its path.
  - size: M

- [ ] M366.P1.T2 — Write the design and get it approved
  - files: `docs/pixel-aspect.md`, `## Decisions` here
  - approach: from the audit, propose either keeping RoI and cache keys in pixel space with no pixel aspect conversion (pixel aspect only scales canonical coordinates for display and transforms), or the narrowest fix that keeps today's model. Name the GPU contract change each implies. Send it to the user (`response`) and record the answer.
  - verify: the user's choice is recorded in `## Decisions`.
  - size: L

## Phase 366.2: Fix

- [ ] M366.P2.T1 — Reproduce the cache-dependent tile
  - files: `Tests/` (a new `PixelAspectTile_Test.cpp`, registered in `Tests/CMakeLists.txt`)
  - approach: a graph with a pixel aspect 1 source feeding a Crop to a pixel aspect 2 output; render one tile with an empty cache and again after a larger entry is cached.
  - verify: the test fails on the default branch, the two renders differing outside `bounds ∩ RoI`.
  - size: M

- [ ] M366.P2.T2 — Apply the approved design
  - files: the sites P1.T1 lists that the design changes, `docs/pixel-aspect.md`, the GPU contract doc
  - approach: implement the choice recorded in P1.T2 and update the GPU contract doc in the same change. If the design touches more than five files, split this task per subsystem (RoI, cache keys, deep) before dispatch.
  - verify: `PixelAspectTile_Test` passes with both cache states giving identical pixels; the cache and RoI suites still pass.
  - size: L

**Verification gate:** full ctest passes, `PixelAspectTile_Test` included, with no golden image changes except ones the design note predicts and the user accepted.

## Decisions

- 2026-11-07 — Its own milestone, design first (user): pixel aspect is display metadata and a pixel is still a pixel, so question the conversions before patching.
