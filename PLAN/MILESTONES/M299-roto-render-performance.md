# M299 - Roto Render Performance

Follow-up from the user's 2026-11-01 UAT of M224 - Fluid Roto And RotoPaint Interaction. Handle interaction with many shapes is good now, but Roto and RotoPaint render slowly. This milestone makes the nodes render faster, with caching where it pays: per-shape and per-stroke masks, reused across frames for shapes that don't animate. It builds on M276 - Upstream Reuse On Edits and M300 - Unified Tiered Cache. It profiles first (rasterisation, feather, merge tree, per-render setup), and that profile decides which candidates in Phases 299.3 and 299.4 run.

Deps: M224, M276, M300.

What the code does on main (c97bbfcb6; line numbers approximate):
- **Mask render:** each item's mask comes from `RotoDrawableItem::renderMaskFromStroke` (`Engine/RotoContext.cpp` ~1225), called from the item's effect node input fetch (`Engine/EffectInstance.cpp` ~2236).
- **Cache key:** the mask's key always carries the frame (`frameVaryingOrAnimated = true`, ~1247), so a still shape is rasterised again on every frame. The key's hash is the item's Merge node render hash (~1238), so an upstream edit probably invalidates every mask; P1.T3 confirms this.
- **Coverage:** the mask covers the shape's whole bounding box whatever the RoI (~1323).
- **Raster:** the fill and feather run as serial row tiles on one thread (`RotoContextPrivate::renderBezier` ~1832, tile loop ~1885; `kBezierTileArea` ~1821 makes most shapes one tile). Strokes go through cairo dabs (`renderStroke` ~1656).
- **Merge tree:** each item adds a Constant and a Merge node to the RotoPaint tree. One shared Merge collects them when the tree can be concatenated (`refreshRotoPaintTree` ~3195, `RotoPaint::render` `Engine/RotoPaint.cpp` ~822).
- **Recompute cost:** masks are stored outside the scheduler, so they never get M300's measured recompute cost. The host tier falls back to an unmeasured default (`Engine/CacheHostTier.cpp` ~74; the scheduler sets the cost at `Engine/EffectInstanceRenderRoI.cpp` ~177).

Coordination with M370 - Roto Point List Render Isolation, which runs in parallel: M370 owns the point list, the `broken` and `finished` flags and `Bezier::clone` in `Engine/Bezier.cpp`/`Engine/BezierCP.*`. M299 reads geometry only through the render-side accessors (`getControlPoints_mt_safe`, `evaluate*_DeCasteljau`) and doesn't change point storage in those files. If a task must touch `Engine/Bezier.cpp`, it keeps the change to a read-only query and rebases on M370.

## Scope

**In:**
- A Roto/RotoPaint render-throughput bench with a recorded baseline, plus per-stage render timers.
- Mask cache keys that survive upstream edits and, for items that don't change over time, survive frame changes.
- Masks stored with their measured raster cost, so M300's eviction keeps the expensive ones.
- Whichever raster, merge-tree and setup optimisations the Phase 299.1 profile ranks worth doing (candidates below).

**Out:**
- Point list, `broken`/`finished` flag and `clone` isolation (M370 - Roto Point List Render Isolation).
- GPU rasterisation of shapes and strokes through M472's raster pipelines. No milestone owns this yet; file one if the profile shows CPU raster still dominates after Phase 299.3.
- Tiled rendering in general (M64 - Tiled Rendering).
- Feather look changes. M309 - Roto Feather Falloff Banding is done, and its output stays bit-identical.
- The live-stroke path while painting (`Node::getOrRenderLastStrokeImage`, M224's coalescer). It stays unchanged unless the profile names it.
- Frame reuse of the RotoPaint output or of its Merge tree as a whole (general engine frame-variance).

**Edge cases:**
- Inverted shapes: the mask depends on the source RoD (`rotoNodeSrcRod`), so that RoD joins the key.
- Motion blur (`NATRON_ROTO_ENABLE_MOTION_BLUR`) makes an item frame-varying, and so does any time-dependent input: keyed points or feather, keyed render-hashed knobs, expressions that read the frame or other knobs, and track-linked points.
- Lifetime and activation: an inactive item is already an identity (`EffectInstance.cpp` ~5597), so frame reuse needs no lifetime term.
- An edit or abort mid-raster: keep the `CleanHashSnapshot` / `allowsStoreUnder` drop and the remove-on-partial behaviour (~1356, ~1400).
- Parallel tiles: pool threads must not read `BezierCP`s, because M304 picks the render side per render thread. Evaluate all geometry on the render thread, then hand out plain data.
- Clone, reveal, smear and blur strokes: the mask can be reused, but the effect node still re-renders with its input.
- Proxy and mip levels, 8-bit/16-bit/float depths, one-channel and RGBA components: all must stay in the key (M300 completed `ImageKey`).

**Assumptions:**
- The Merge node's render hash includes its upstream inputs' hashes. P1.T3 verifies this; if it is false, P2.T1 shrinks to the frame-invariance part.
- `parallelForOnGlobalPool` (`Engine/PoolParallelFor.h`) is safe to call from a render thread for raster bands, as the M153 field solve does.

## Phase 299.1: Profile and baseline

- [x] M299.P1.T1 — Add a Roto render-throughput bench
  - files: tools/bench/roto_render_bench.py (new), tools/bench/roto_render_bench.sh (new), tools/bench/gui/make_roto_strokes.py (new), tools/bench/README.md
  - approach: A NatronRenderer script builds Roto and RotoPaint scenes through the Python roto API:
    - scenes: N in {1, 30, 100, 300} closed shapes of 24 points; feather 0 or 20 px; still or keyed on every frame; at 2K and UHD; over a Read plate;
    - output: renders frames 1..F and reports one JSON line per scenario with the cold first frame, the median and p95 of later frames, and two edit-then-rerender cases (one shape's colour changed; one upstream Grade changed);
    - strokes: the Python API can't create strokes, so `make_roto_strokes.py` reuses `tools/bench/gui/roto_bench.py`'s stroke driver under Xvfb to paint 50 and 300 solid strokes, and saves a small project with no images in it for the bench to load.

    Emit records that `tools/bench/compare.py` can diff; check its input format first.
  - verify: The script runs against a release AppImage under llvmpipe and prints a well-formed line per scenario. The README documents the environment variables.
  - size: M

- [x] M299.P1.T2 — Add per-stage Roto render timers behind an environment variable
  - files: Engine/RotoContext.cpp, Engine/RotoPaint.cpp, Engine/RotoLatencyProbe.h/.cpp (reuse its clock and log writer if it fits)
  - approach: When `NATRON_ROTO_RENDER_PROFILE=<path>` is set, write one JSON line per mask and per RotoPaint render with steady-clock stage times:
    - cache lookup, hit or miss;
    - polygon evaluation;
    - feather quads and rasteriser setup;
    - fill;
    - feather raster;
    - conversion;
    - the tree render (`renderInputOrTakeFromStore`);
    - setup (`getCurvesByRenderOrder`, tree nodes).

    When the variable is unset, the cost is one branch.
  - verify: A gtest renders a 2-shape Roto with the variable set and checks that every stage appears once per mask. RotoContentHash_Test and RotoKeyDuringRender_Test still pass.
  - size: M

- [x] M299.P1.T3 — Record the baseline and profile, then pick the later tasks
  - files: tools/bench/baselines/roto-render.jsonl (new), tools/bench/BASELINE.md, this file's `## Decisions`
  - approach: On the GPU host or a cloud VM (not GitHub runners), run the P1.T1 bench on the merged P1 build with P1.T2's timers on. Then sample stacks (`tools/bench/sample_stacks_gdb.sh` + `analyze_stacks.py`) on the 100-shape UHD still and keyed cases and on the 300-stroke case. Confirm or refute the Assumptions. Rank rasterisation, feather, merge tree and per-render setup by share of frame time. Record in `## Decisions`:
    - which Phase 299.3/299.4 tasks run, and which are struck and why;
    - the speedup targets for the gate. Proposed: at least 3x on later frames of the 100-shape still UHD case, at least 1.5x on its cold frame, and no case slower than 1.05x.
  - verify: The baseline file and the ranked profile are committed. `## Decisions` names each candidate as kept or struck.
  - size: M

## Phase 299.2: Mask cache reuse

- [x] M299.P2.T1 — Key masks on the item's own content, not the Merge node's render hash
  - files: Engine/RotoContext.cpp (renderMaskFromStroke ~1225), Engine/RotoDrawableItem.h/.cpp
  - approach: Build the mask hash from:
    - the item's content hash (`RotoItem::getContentHash` with `appendLiveContentHash`, `Engine/RotoDrawableItem.cpp` ~681);
    - the shape geometry hash;
    - the RotoPaint node's mask-relevant state (layer, output components);
    - the source RoD, only when the shape is inverted.

    Keep it salted the way `ContentHasher::withSaltFlag` is today, and keep the clean-snapshot store guard. Upstream edits then reuse every mask.
  - verify: A new gtest (Tests/RotoMaskCache_Test.cpp) counts raster calls:
    - an upstream Grade edit re-renders zero masks;
    - editing one shape re-renders one;
    - an inverted shape re-renders when the source RoD changes;
    - outputs match a cold render bit for bit.
  - size: M

- [x] M299.P2.T2 — Reuse masks across frames for items that don't change over time
  - files: Engine/RotoContext.cpp, Engine/RotoDrawableItem.h/.cpp, Engine/RotoStrokeItem.cpp, Engine/Bezier.cpp (read-only query only; see the M370 note)
  - approach: Add `RotoDrawableItem::isMaskFrameInvariant()`. It is true when the item has:
    - no keys on points or feather (`Bezier::getKeyframesCount`);
    - no animated render-hashed knob;
    - no expression or outside-reading knob (`getContentHashDependencies`);
    - no track links;
    - no per-shape motion blur.

    Strokes are invariant unless a knob is animated. Pass `frameVaryingOrAnimated = !invariant` to the `ImageKey`, and cache the answer per content-hash generation so it isn't recomputed per frame.
  - verify: Tests in RotoMaskCache_Test:
    - a still shape rendered at frames 1..5 rasterises once;
    - a keyed shape, an opacity expression on `frame`, a track-linked point and adding a key each rasterise per frame;
    - outputs match per-frame renders bit for bit.
  - size: M

- [x] M299.P2.T3 — Store masks with their measured raster cost
  - files: Engine/RotoContext.cpp, Engine/CacheEntry.h (read), Engine/CacheEviction (read)
  - approach: Time `renderMaskInternal` and write `recomputeCostNs` on the entry header, the way `EffectInstanceRenderRoI.cpp` ~177 does, so M300's score keeps expensive masks over cheap upstream tiles.
  - verify: A gtest reads the header after a mask render and finds a cost above zero. RotoMaskCache_Test still passes.
  - size: M

## Phase 299.3: Raster speed (run the tasks P1.T3 keeps)

- [x] M299.P3.T1 — ~~Rasterise Bezier fill and feather in parallel row bands~~ (struck by P1.T3's profile; see Decisions)
  - files: Engine/RotoContext.cpp (renderBezier ~1832, renderInternalShape ~2727, kBezierTileArea ~1821), Engine/RotoFeatherRaster.cpp
  - approach:
    1. On the render thread, evaluate each sample's fill path into plain points (no `BezierCP` reads off-thread) and build the `RotoFeatherRasterizer`.
    2. Split the RoI into bands sized for the core count, with a minimum band height so small shapes stay serial, and run them with `parallelForOnGlobalPool`.
    3. Give each band its own A8 cairo surface and feather pass, and call the sink per band.
    4. Check for abort between bands.
  - verify: BezierEdit goldens (Tests/fixtures/native-golden/BezierEdit), RotoFeatherBanding_Test and BezierGuiCopyEquivalence_Test stay bit-identical. A gtest renders the same shape with 1 and 16 bands and compares them bit for bit.
  - size: L

- [x] M299.P3.T2 — ~~Rasterise only the requested window of large masks~~ (struck by P1.T3's profile; see Decisions)
  - files: Engine/RotoContext.cpp (renderMaskFromStroke ~1323), Engine/EffectInstance.cpp (~2236, pass the RoI)
  - approach: Run this only if P1 shows that whole-bbox masks far larger than the RoI cost real time (zoomed viewer, tiles). Pass the pixel RoI into the mask render and rasterise RoI ∩ bbox. Record the rendered rows or tiles in M300's rendered-pixel bitmap, so later requests fill only what's missing.
  - verify: A gtest renders two disjoint RoIs, then the full bbox, and matches a one-shot render bit for bit, rasterising each pixel once. The M300 partial-entry tests still pass.
  - size: L

- [x] M299.P3.T3 — Rasterise strokes in parallel row bands (bb5d6e6ff)
  - files: Engine/RotoContext.cpp (renderStroke ~1656, renderMaskInternal's cairo path ~1405)
  - approach: Run this only if P1 ranks stroke raster high. Pre-evaluate the dab list on the render thread, then render bands in parallel. Each band is clipped to its rows on its own cairo surface and keeps the dab order and its own pressure-level pattern cache.
  - verify: Tests/Serialization/RotoPaintReference_Test and the M224 stroke golden stay bit-identical. A 1-band vs many-band gtest matches bit for bit.
  - size: M

## Phase 299.4: Merge tree and per-render setup (run the tasks P1.T3 keeps)

- [x] M299.P4.T1 — ~~Cache the render-order item list and tree node list per context age~~ (struck by P1.T3's profile; see Decisions)
  - files: Engine/RotoContext.cpp (getCurvesByRenderOrder, getRotoPaintTreeNodes, getRotoPaintBottomMergeNode ~158), Engine/RotoContextPrivate.h
  - approach: Keep the lists, keyed on the context age that `makeStroke`, insert, remove and reorder already bump, so each render stops walking layers and doing dynamic casts per item.
  - verify: A gtest checks that inserting, removing, reordering and changing an operator each refresh the list. RotoLayer_Test and RotoSelectionProxy_Test pass.
  - size: M

- [x] M299.P4.T2 — Composite concatenatable trees directly in RotoPaint::render
  - files: Engine/RotoPaint.cpp (render ~822), Engine/RotoContext.cpp, Engine/Nodes/Merge/Merge.h (reuse its operator kernels)
  - approach: Run this only if P1 shows that the per-item Constant + Merge renders dominate. When `isRotoPaintTreeConcatenatable` holds (Beziers and solid strokes, one operator), fetch each active item's cached mask and composite colour × mask onto the background with the Merge operator's own per-pixel function, skipping the internal nodes. All other trees keep the node path.
  - verify: A parity gtest renders 30-item trees for each operator in use through both paths and compares them bit for bit across depths and components, including inverted and inactive items.
  - size: L

**Verification gate:**
- Full build and PR CI green (debug asserts and release).
- RotoMaskCache_Test and the new parity and band tests pass. The BezierEdit, RotoFeatherBanding and RotoPaintReference goldens are bit-identical.
- The m224 and m304 GUI scripts pass under Xvfb.
- `tools/bench/roto_render_bench.sh` runs before and after, in one sitting, on the GPU host or a cloud VM (never GitHub runners), and meets the speedup targets P1.T3 records against `tools/bench/baselines/roto-render.jsonl`.
- `tools/bench/gui/roto_bench.py` shows no regression on stroke and add_shape.
- If the diff touches `Gui/NodeGraph*`, `Gui/NodeGui*`, `Gui/Edge*`, `Engine/Knob*` or `Engine/NodeGroup*`, run `tools/bench/gui/run.sh --xvfb` and `compare.py` against `tools/bench/gui/baselines/xvfb.jsonl`, and record the result in `## Decisions`.

## Decisions

- 2026-11-20 — Elaborated from the stub by a consultant after the claim; its deps (M224, M276, M300) are done. GPU rasterisation is Out; a separate milestone can be filed if the Phase 299.1 profile calls for it.
- 2026-11-20 — P1.T1 and P1.T2 in 48f1f34a7. AppManager starts the profile from the environment, and there is an extra `allocate` stage. **PM calls:** the bench uses its own 8-frame checkerboard plate, so decode cost doesn't swamp Roto; compare.py gains the cold, p95 and edit columns, printed only when both files have them. Build running; P1.T3 (VM baseline and profile) follows.
- 2026-11-21 — P1.T3 ran on a Vast RTX 4090 VM (EPYC 7C13 ×30, 49 GB), with the 48f1f34a7 AppImage. The baseline is `tools/bench/baselines/roto-render.jsonl` and the profile is BASELINE.md "Roto render" (committed on the lane). Six RotoPaint 300-shape records are excluded: the persistent disk tier served the identical Roto scene's Merge outputs. The stroke fixtures failed to paint, so there is no stroke data.
- 2026-11-21 — **Profile** (100 shapes UHD, share of frame): Merge compositing 30%, outside RotoPaint 22%, per-item effect nodes 19%, RGBA conversion 14%, feather 4%, fill 2%, setup <0.1%, cache lookup 0.1%. The render runs on about 1.6 of 30 threads.
- 2026-11-21 — **Assumptions:**
  - Assumption 1 holds in the code but the bench hides it. The effect-node cache serves Grade edits with zero mask requests, and there were 0 mask cache hits overall (still shapes rasterise every frame). P2.T1 stays, because P4.T2 bypasses those nodes.
  - Assumption 2 is untested, and nothing kept relies on it.
- 2026-11-21 — **Keep and strike (PM, from the profile):**
  - Kept: P2.T1–P2.T3, and P4.T2, which composites single-channel masks with colour applied in the composite.
  - Struck: P3.T1 (raster is about 8.5%; parallelism across items is the fallback if the targets miss), P3.T2 (no case where the RoI is smaller than the mask) and P4.T1 (setup 0.33 ms).
  - Deferred: P3.T3, until strokes are re-run.
- 2026-11-21 — **Gate targets (PM, replacing the proposal):**
  - 100-shape still UHD: later frames ≥3x, cold ≥1.75x.
  - 100-shape keyed UHD: later frames ≥1.5x.
  - Edit-Grade and edit-colour: ≥1.5x each.
  - No median slower than 1.10x (run-to-run noise is ±7%).

  Before the gate, the bench gets a fresh `NATRON_DISK_CACHE_PATH` per scenario, and make_roto_strokes.py is fixed for the stroke re-run.
- 2026-11-21 — Phase 299.2 committed. The mask key is the item's content hash plus the Merge node's restricted render-context hash. `isMaskFrameInvariant()` is cached per content hash. The measured raster cost goes into recomputeCostNs. The bench turns the disk tier off (`NATRON_DISK_CACHE_PATH` doesn't move the tier). make_roto_strokes.py had never run, because guitest wasn't on sys.path; that's fixed. **PM call:** a point with exactly one keyframe counts as still. Gate running; then the stroke fixtures, then Phase 299.4's P4.T2.
- 2026-11-21 — **Phase 2 VM bench** (RTX 4090 VM with 5 vCPUs, cloud-gpu-20261121T140413Z; before 48f1f34a7, after 13aa297b5): still later 1.32x, cold 0.99x, edits 1.00x, keyed later 1.01x; strokes skipped (make_roto_strokes found no ViewerGL). Consultant: the bench is valid and the cache hits (RSS 20.3 → 15.3 GB on still). 1.32x is about Phase 2's ceiling: masks were 27% of the frame.
  - **Re-plan (PM):** the 3x / 1.75x / 1.5x targets are the end-of-milestone gate, applied after P4.T2. Phase 2 is accepted: still later ≥1.25x, nothing else slower beyond noise. P4.T2 composites in parallel. Across-item parallel raster (struck P3.T1's fallback) comes back only if cold misses after P4.T2.
  - **Bench fixes before the next VM run:**
    - a profiled pass for hit counters;
    - the build hash in each log;
    - a VM with ≥30 vCPUs like the baseline;
    - 5 reps on edit cases;
    - the stroke fixture fails the run instead of being skipped silently.
  - P4.T2 dispatched.
- 2026-11-21 — **P4.T2 design notes** (agent paused for a container restart; nothing written yet):
  - **PM calls:** the direct path fetches the same RGBA float masks the node path uses, and shares its cache entries. A single-channel mask stores coverage×opacity, so it can't be bit-identical. Non-RGBA trees and any A edge mode other than Black fall back to the node path, as do mix ≠ 1, a connected mask, partial channels and a stroke in progress.
  - **Reference points:**
    - Roto output = overFunc in OFX Roto.cpp ~245-290; keep -0/NaN with `float(double(m)+0.0*(1-double(a)))`.
    - The fold copies NativeMergeOp::step (Merge.cpp 550-593) and the runStagesCpu band loop (StagePlan.cpp 1410-1560): B alpha broadcast, dstAlpha carry, the isIdentityForTransparentA memcpy shortcut, and a re-read after every 64 A inputs.
    - Output covers roi ∩ the bottom Merge's RoD, and the existing tail (RotoPaint.cpp 909-994) fills the rest.
    - Band-parallel via makeStageRowBands and parallelForCancellable.
    - NATRON_ROTO_INVERTIBLE and the motion-blur defines are compiled out.

- 2026-11-24 — P3.T3 run without the stroke profile it was deferred on: the lane had stalled with nothing else to do; band equality tests (any band count byte-identical to one surface) guard it. Pattern reuse is keyed on exact radii and stops, not on rounded pressure, which would change output.
