# Native geometry viewer flash: root-cause research (code reading)

Tree read: `/home/bosley/git/Natron/.worktrees/m615` (11e3a1b115). All paths below are relative to it. SceneViewport.cpp, GeoDisplay.cpp, SceneDrawFilter.cpp, GeoSdfFileFormat.cpp, GeoViewerLayer.cpp, GeoSdfData.cpp, GeoUsdConvert.* and ScenePresentPacer.h are byte-identical to origin/feature/f15-native-geometry, which the M617 spike read. The m615 tree differs in ViewerInstance.cpp (scene renders now carry the abort info, ~4098), EffectInstanceRenderGeometry.cpp (abort and context plumbing) and ViewerTab.cpp (lines shifted).

Nothing was built or run. The OpenUSD v26.08 source is not on this host, so claims about UsdStage and UsdImaging change handling come from my memory of upstream and are marked **(USD, unverified)**.

## 0. Summary

| # | Candidate | Verdict | Kind |
|---|---|---|---|
| A | An older cook publishes after a newer one: the geometry jumps back a step | **Confirmed: the guard is missing.** Whether it fires needs measuring | fixable bug |
| B | A failed cook blanks the view, then the next good one shows the composing placeholder | **Confirmed path.** Unlikely for Box/Scatter drags | fixable bug |
| C | Native-instance instancer re-added: every instance is re-admitted over several frames | **Confirmed logic bug** (packed native mode only) | fixable bug |
| D | Grey cue at 100 ms on the Geometry Spreadsheet and Prim Inspector, per step | **Confirmed** to toggle per step on slow steps. Not the 3D view itself | design choice / tuning |
| E | A non-Storm viewer renderer (Cycles/MoonRay) restarts its refinement on every refresh | **Confirmed.** Noisy or blank frames per step | inherent to progressive renderers; cue it |
| F | A partition vanishing or appearing (count 0, new paths) goes through admission | **Confirmed.** One empty or partial frame | minor bug |
| G | Storm frames not double-buffered / partial frames shown | **Refuted.** The pacer and fences are correct | - |
| H | Points-only edits resync | **Refuted** at the Sdf level. Property-only resync for topology edits needs measuring | - |
| I | Stand-in boxes during knob drags | **Refuted.** They need the camera to be moving | - |
| J | Viewer stale cue toggling | **Refuted for the 3D view.** It is drawn only on the hidden 2D ViewerGL | - |
| K | Composing or blank placeholder during a steady drag | **Refuted** (`composesInPlace`), except after B | - |
| L | 2D/composite path shown in between | **Refuted.** ViewerGL is hidden. Black's texture disconnect is invisible | - |
| M | Prim paths or topology change for GeoTransform | **Refuted.** Points, normals and extent are info-only field sets | - |

No code path was found that presents a grey/background Storm frame during a steady, error-free GeoTransform drag with Storm on the render thread. If the user sees grey frames in that exact setup, they come from outside the reasoning here: Qt composition, a non-threaded-GL fallback, or a different renderer. Section 4 says how to tell.

## 1. Drag path, step by step (m615)

1. Knob edit, then a viewer request. During a slider drag, `markAllOnGoingRendersAsAborted` keeps the **oldest** running render and aborts the rest (`Engine/ViewerInstance.cpp:5253-5275`: `keepOldest = isDraftRenderEnabled() || ...`). So two scene cooks can be in flight at once: the oldest one and the newest one.
2. `renderViewer_internal`: for a scene input it installs the abort info (m615, ~4098), then calls `evaluateSceneInput` (3982) and `evaluateGeometryInput` (4041), which calls `evaluateGeometry` (EffectInstanceRenderGeometry.cpp).
3. An aborted cook returns Redraw via `renderEndedWithoutImage` (3995) and publishes nothing. A failed cook stores an **empty** `lastScene` plus an error (4026-4029). Either way `sceneEvaluated` is emitted (4032) and the code returns Black (4037).
4. `layer->serveFrame` (4074; `GeoViewerLayer.cpp:160-210`) writes `held[frame] = detail` **unconditionally**.
5. On the main thread, `ViewerTab::showEvaluatedScene` (`Gui/ViewerTab.cpp:1760`):
   - on an error it calls `setError` (1795);
   - otherwise `showGeometryFrame` → `GeoViewerLayer::showFrame` (212-243) consumes `held[frame]` into `slot->detail`;
   - then `setScene` (1814).
6. `SceneViewport::setScene` (`Gui/SceneViewport.cpp:10958`): the stack is the same, so keepsFrame is set. `requestSync` (9663) queues the payload for the worker (9520-9610).
7. The worker runs `syncScene` (4084-4200). `sceneStage->isSyncedTo(staged)` holds, but `GeoDisplay::hasPendingEdits` (`Gui/GeoDisplay.cpp:675-705`) sees a new detail, so it takes the exclusive lock:
   - `sceneStage->sync` is a no-op;
   - `GeoDisplay::editStage` (707) calls `GeoLayers::refresh` (`GeoSdfFileFormat.cpp:591-766`);
   - then it rebuilds the guides (GeoDisplay.cpp 757-940);
   - `noteLayerEdit` runs, and `restart` is set, so `progressive->invalidate()` follows (4185-4187).
8. `onSynced` (9706) calls `startBackgroundFrame` (9905). The render thread runs `renderBackgroundFrame` (4639-4813): Hydra ApplyPendingUpdates inside `engine->Render`, the draw filter, grade and selection, then a fence. `ScenePresentPacer` (`Gui/ScenePresentPacer.h`) flips front/back only once the fence has signalled. `paintGL` (12757) blits `frontFrame` via `presentRenderTarget` (9247-9289).

## 2. Hypotheses

### A. An older cook overwrites a newer one (CONFIRMED missing guard; likelihood: medium-high during slider drags)

The image path drops late frames with `checkAndUpdateDisplayAgeIfNewest` (`Engine/ViewerInstancePrivate.h:442-455`). The scene/geometry path has **no render-age check** before:
- `serveFrame` (`ViewerInstance.cpp:4074`);
- `lastScene[...] = ...` and `Q_EMIT sceneEvaluated` (4024-4032).

`serveFrame` overwrites `held[frame]`, and `showFrame` takes whatever is there.

The slider path deliberately keeps the oldest render alive (5255-5266). So the following sequence happens:

| Time | Event |
|---|---|
| t0 | Render R1 (value v1) starts. It is the oldest and is kept. |
| t1 | Render R5 (value v5) starts. R2-R4 are aborted. |
| t2 | R5 finishes first: upstream is cached, or it runs on another core. `held = v5`, it is shown, then synced. |
| t3 | R1 finishes. `held = v1`, `showFrame` reports changed, `setScene` runs, then refresh and a new frame. **The geometry snaps back to v1.** |
| t4 | The next request shows v6. |

On screen this is a back-and-forth jitter or "flash" of the earlier pose.

There is a variant. If R1's `serveFrame` lands between R5's `serveFrame` and the GUI processing R5's signal, v5 is never shown. The drag can then rest on an earlier value until the release render.

Before m615 the abort never reached scene cooks, so every stale cook ran to completion. That made this race much more likely in the F15 UAT build.

**Fix:** carry the render age into `serveFrame` and `lastScene`, and drop publishes older than the newest one already shown (`checkAndUpdateDisplayAgeIfNewest` semantics, but publishing whenever the age is newer than `displayAge`, not only for the newest request). Alternatively, compare the node hash read before the cook (`sourceHash`, already captured at 4048) with the current hash and drop the result if it is outdated.

### B. A failed cook blanks, then the composing placeholder shows (CONFIRMED path; likelihood for Box/Scatter: low)

`ok == false` stores an empty scene (4026), and `setError` (`SceneViewport.cpp:11037`) syncs an empty payload. The worker then sets `shown=false` (4182), and `paintGL` treats `blank` (12788) as a background clear plus the red error text (12866-12876, 13013).

The next good cook calls `setScene`, but `requestedPayload` is now empty. So:
- `keepsFrame=false`, which calls `stopBackgroundFrame` (9690);
- `composesInPlace=false` (9668), so `composingShowsStatus()` holds (8981) and the "Composing" status is painted (13017);
- the stage stack is re-appended. `sync` returns true, every geo prim is recomposed (resynced), and the prims come back through the draw filter.

The filter's `_removedHeld` only spans one render (`SceneDrawFilter.cpp:497-499, 1249-1252`). The removal happened in an earlier render, so the re-added prims count as new and **queue for admission** (1310-1320). They then fill in over several frames.

That full sequence is a grey frame, then the placeholder, then partial fill.

In m615, an aborted cook maps to Aborted, not Failed, at every return in `evaluateGeometry`. So this needs a genuine node error: an expression error mid-drag, GeoWrangle or Python code, or a transiently invalid knob combination.

**Fix:** on failure, keep the last good scene and show the error as an overlay only.

### C. Native-instance instancer re-add re-waits every instance (CONFIRMED logic; applies to packed native mode, e.g. copy-to-points)

`readInstancer` (`SceneDrawFilter.cpp:522-575`) keeps each instance's admitted state only from the **current** `_instancers` record. A remove+add of the instancer (a resync) goes like this:
1. `forget()` calls `forgetInstancer()` (489-491), which erases that record.
2. `_PrimsAdded` calls `readInstancer` (1293-1294) with an empty `waited` map, so every instance is set waiting (569-572).
3. The instances are masked off and re-admitted at 64 per prim-unit per frame.

`_removedHeld`, which protects ordinary rprims, is not consulted for instances.

Result: on any instancer resync the instanced copies blink out and fill back in. Point instancers carry no instanceLocations (`locations.empty()` returns early at 556), so the default packed mode is unaffected.

**Fix:** consult `_removedHeld` (or keep a per-render snapshot of the forgotten instancer's waiting map) in `readInstancer`.

### D. Grey cues in the panels toggle per step (CONFIRMED)

The Geometry Spreadsheet greys its text when a read takes longer than `kGreyAfterMs = 100` (`Gui/GeoSpreadsheetPanel.cpp:58`, 313-315, 438-442; `GeoSpreadsheet.cpp:994-1005`). The Prim Inspector does the same (`Gui/ScenePrimInspector.cpp:89`, 1476). Every drag step longer than 100 ms greys them, and they ungrey when the read lands.

This is not the 3D view. A user with either panel docked beside the viewer could still describe it as "grey flashing". Ask the user.

**Fix:** keep the panels grey for the whole interaction (hysteresis), as the 2D viewer's stale-cue timing does (`Gui/ViewerStaleCueTiming.cpp`).

### E. A non-Storm viewer renderer restarts every step (CONFIRMED)

`editStage` returns `renderChanged=edited` (GeoDisplay.cpp 727-729). Then `restart` is set and `_imp->progressive->invalidate()` runs (SceneViewport.cpp 4185-4187). With Cycles or MoonRay as the viewer renderer, each refresh restarts sampling, so frames go back to noisy or empty. This only applies if the user's viewer is not on Storm. Check the renderer row.

### F. A partition appears or vanishes during the drag (CONFIRMED, minor)

A new prim path, a count dropping to 0 and back, or a partition whose path changes is erased in one sync and re-added in a later one. It is then "not known" and not in `_removedHeld`, so it **queues** (SceneDrawFilter.cpp 1310-1320). It is admitted on the next frame's `admit()` (SceneViewport.cpp 2358-2368), and at least one unit comes in per call.

Result: one frame without that prim. GeoScatter with count>0 keeps `/geo/points_0`, so this does not happen in the normal case.

### G. Storm double-buffering (REFUTED)

- **Back target.** `startBackgroundFrame` renders into `presentPacer.backTarget()` (9932), which is always the non-front target (ScenePresentPacer.h 68-71).
- **Gating.** It is gated on `!presentPacer.pending()` (9909).
- **Front flip.** The front only flips after the fence has signalled (`poll()`, 102-113).
- **Failed frames.** A frame that fails, or renders 0 passes, gets no fence and is never submitted (4771-4772, 9949-9951). The `clearFrameTarget` at 4653 touches only the back target.
- **Partial frames.** Storm cannot stop mid-pass. A frame ended by `exclusiveWaiting` stops between full `Render` calls (4664-4670), so every presented frame is a complete Storm render.
- **Qt.** `paintGL`'s early returns (12774, 12816-12823, 12886-12897) rely on `PartialUpdate` (9340). The code comment there says this was "not checked against Qt's source". In Qt, QOpenGLWidgetPrivate::render() only calls invalidateFbo() for NoPartialUpdate, and paintEvent skips render() while updates are disabled. So the FBO keeps the last frame **(Qt, from memory; verify against the Qt 6.8 source)**.

### H. Resyncs on points-only edits (REFUTED at Sdf level; topology edits need measuring)

`GeoLayers::refresh`:
- A partition whose signature changed but whose topology did not is diffed field by field and edited with `SetField` (713-735). That is an info-only change on property paths, so no resync **(USD, unverified)**.
- `GeoSdfData::StreamsData()` returns false (GeoSdfData.cpp 97-103).
- Nothing calls `SdfLayer::Reload` on geo layers. A grep shows Reload only on file layers (SceneOps.cpp ~736).
- `natronGeoRefreshResyncs` is asserted to stay 0 in m376_geometry_display.py:198 and m376_geo_playback.py:210.

Topology changes (GeoScatter count) behave differently. `topologyChanged` (437-455) erases and re-authors every property **and child prim** under the partition (`underRebuilt`, 666-674, 695). UsdStage reports those as resynced *property* paths (and child-prim paths for GeomSubsets) **(USD, unverified)**.

In UsdImagingStageSceneIndex, property resyncs should become dirties through `InvalidateImagingSubprim(..., Resync)` rather than a remove+add of the gprim **(USD, unverified; measure)**. If it did remove and re-add, both would land in one `ApplyPendingUpdates`, so `_removedHeld` keeps the prim drawn. The cost is then a Storm rprim and batch rebuild per step, not a flash.

Edits stay in the data whenever `sameOrder` fails (742-763). That disables the `kept` and content-hash fast paths for every later cook (630, 654), and every field read pays the `findEdit`/`ancestorHidesBase` map walks (GeoSdfData.cpp 105-133).

### I. Stand-in boxes (REFUTED for knob drags)

`updateStandInLatch` latches only when `overBudget && viewMoving()` (8879-8888), and `viewMoving()` depends on `paintedView` (camera and size, 8946-8952). A knob drag never moves the view. A drag combined with a tumble, or `eSceneStandInModeAlways`, would show the boxes.

### J. Stale cue (REFUTED for the 3D view)

`staleChanged` → `ViewerTab::onViewerStaleChanged` (`Gui/ViewerTab10.cpp:342-379`) → `_imp->viewer->setStaleCue`, which is the 2D ViewerGL. SceneViewport has no stale cue; the only "stale" in it is a comment at 5007. In scene mode the ViewerGL is hidden (ViewerTab.cpp ~1703).

### K. Composing or blank placeholders (REFUTED in a steady drag)

`composingShowsStatus()` = `!presentedScene || !composesInPlace || rendererSwitchRunning` (8981-8984). A drag keeps the same stack, so `composesInPlace` stays true and m559 asserts that no composing paints happen. Placeholders appear only after B, after a stack change, or after a context loss (`releaseGLResources` resets `frontFrame`, 9204-9213).

### L. 2D/composite path in between (REFUTED)

Black → `disconnectTextureAfterRender` (ViewerInstance.cpp 3216-3221, 6236-6249) only touches the hidden 2D ViewerGL. `activeInputDataKindChanged` fires only when the input's kind changes (6807-6816), which a knob drag does not do. If it ever did, `geometryLayers` would be reset (6794-6796) and a new layer identifier would force a full restage.

### M. GeoTransform changes paths or topology (REFUTED)

GeoTransform rewrites P/N-like attributes in place and adds no attributes, groups or `path`/`name` (a grep for addAttribute, "path" and addGroup in GeoTransform.cpp finds nothing). The partition paths and signatures of the topology columns are unchanged. Only point-page hashes differ, so only the point-dependent fields are diffed.

### Not found but worth noting

- **Transform preview snap-back.** A viewport-handle drag on a scene transform clears its preview in the sync that lands (`clearPreviewLocked`, 4144-4147; `previewClearAfter`). That is not the knob path.
- **Policy/draw-rule holds.** `holdStageUntilMatch`, which sets `stageHeld` and therefore `blank`, runs only for the first sync of a newly viewed node (9711-9716).

## 3. Is the design fragile or merely buggy?

The Sdf file-format-plus-diff design is not what flashes. Points-only steps author info-only fields, and the viewer's double buffering is correct.

The confirmed causes A-C and F are bugs in viewer orchestration:
- an unguarded publish order;
- error handling that empties the stage;
- the draw filter's per-render memory of what Hydra held.

All of them are fixable locally.

The design is fragile in other ways:
1. **Correctness depends on discipline across many cooperating flags** (`composesInPlace`, `keepsFrame`, `_removedHeld`, `presentedScene`, `paintHeldUntilSerial`, `stageHeld`, admission). Each new path has to get them all right. A and C are both "state kept for exactly one window" bugs.
2. **Ordering subtleties disable the fast path.** Once `sameOrder` fails, the data keeps its edits forever and the `kept` and content-hash fast paths are gone.
3. **Topology edits are erase+re-author under the prim**, so they depend on UsdImaging's property-resync handling.
4. **Every step costs O(N) serial work in several passes** (below), which is the real scalability limit on heavy geometry.

Only point 4 argues for the option-3 scene index. The flashes do not.

## 4. Per-step cost (points-only GeoTransform on a mesh with N points, V face-vertices, P faces)

All of it is serial, with no TBB in refresh, the convert or GeoTransform's loops.

| Stage | Where | Passes |
|---|---|---|
| Cook | GeoTransform.cpp 249-260, 329-345 | 1×N for P, plus 1×N per vector/normal attribute |
| Cache store | fetchGeometryThroughCache | O(1), plus allocateMemory |
| `getSizeInBytes` | GeoViewerLayer.cpp 190 | O(#attrs) |
| Content hash compare | refresh 630 | page hashes (cached per page?) |
| New `GeoUsdConvert` build | GeoUsdConvert.cpp `build` 747 | O(P) partitioning plus spec planning |
| `computeSignature` | 1611-1680 | O(P+V) loop building vertex/point offset vectors, `pagesOf` (sort/unique, O((P+V) log)), page hashing per attribute |
| `buildValues` (`after`) | 1738+ | O(N) points to VtVec3fArray, O(V) faceVertexIndices, O(P) counts, O(N or V) per primvar, O(N) extent |
| `topologyChanged` | GeoSdfFileFormat.cpp 437-455 | full O(V)+O(P) compares of index/count arrays (different buffers, so no IsIdentical shortcut) |
| Field diff | 713-735 | **second** O(V)+O(P) compare of the same topology arrays; O(V) per unchanged faceVarying primvar (uv); points compare exits early; SdfLayer::SetField compares old vs new again (early exit) |
| Spec visits | 676-683, 744-760 | O(#specs) twice, plus a children-order check |
| Guides | GeoDisplay.cpp 793-940 | rebuilt whenever the detail pointer changes: packed scan O(P); markers O(N) if on; normals O(N) if on; labels O(N+P) if on; bounds O(N) if any guide is on |
| Hydra | UsdImaging dirty, then HdSt reads points through GeoSdfData `getField` (copy-on-write VtValue) | 1×N GPU upload, plus extent and normals |
| Storm render, grade, selection pass, present | | frame cost |
| Panels | Spreadsheet/Inspector re-read if open | O(N·attrs) |

**Roughly 6-9 O(N) or O(V) serial passes per step before Storm**, plus topology compares that scale with V even when only P moved.

Cheap wins:
- Reuse `before`'s topology VtArrays when the topology pages are unchanged. Equal pointers then compare in O(1) and the double compare disappears.
- Skip `topologyChanged` when the topology-column page hashes match.
- Parallelise `buildValues` and the cook loops.
- Rebuild guides only when their inputs change.

## 5. Measurements that would settle the rest

Run them on the m615 AppImage, GeoTransform-drag a Box and a 1-5M-point mesh, and GeoScatter-drag the count.

1. **Publish order (A).** Log `abortInfo->getRenderAge()` and the node hash at `serveFrame` (ViewerInstance.cpp 4074) and at `showFrame`. Any shown age lower than one already shown confirms A.
2. **Presented-frame detector.** In `presentRenderTarget` (9247), read back a 64×64 downsample. Flag:
   - coverage (non-background pixels) dropping by more than 20%;
   - centroid jumps against the trend (that is A).

   Also log every paintGL branch taken: blank-clear 12866, composing 12822/13019, early-hold 12774, context-rebuild 12763.
3. **Draw filter.** Per step, record the `drawStats` deltas `inputRemoved`, `inputAdded`, `queued`/`pendingCount` (`natronSceneDrawStats`). Any non-zero on a points-only step refutes H. A non-zero `pendingCount` after a Scatter count step shows admission fill (F/C).
4. **Resync paths.** Extend `ResyncCounter` (GeoDisplay.cpp 539-560) to log whether each path is a prim or a property, and include the guide-layer edits, which it misses today.
5. **Environment.**
   - Log `usesRenderThread` / `QOpenGLContext::supportsThreadedOpenGL()` and the active renderer id (E, plus the non-threaded path).
   - Ask whether the Spreadsheet or Prim Inspector was open (D).
   - Ask whether the "flash" is grey (B, or Qt composition) or a jump-back (A).
6. **Qt composition.** Grab the window via `QScreen::grabWindow` every 5 ms during a drag and compare it with the widget's `grabFramebuffer`. A grey that shows only in the screen grab is in Qt or the compositor, not in our frames.
7. **Fast-path loss.** Log `sameOrder` and `data->hasEdits()` per refresh (GeoSdfFileFormat.cpp 761).
8. **Cost.** Time refresh as convert build, signature, buildValues, topologyChanged, field diff and Sdf notice. Time guides, the stage-lock wait in `lockExclusive`, and Storm Render.

## 6. Addendum: grey view while dragging GeoScatter Pscale (Storm; spreadsheet and inspector closed)

Paths are relative to `.worktrees/m615`.

### What one Pscale step changes

**1. GeoScatter re-runs the whole scatter, and the result is identical except for pscale.**
- `renderGeometry` calls `scatterGeometry` in full (`Engine/Nodes/Geometry/GeoScatter.cpp:741-759`).
- The samples depend only on seed, count and density, plus the triangles. The RNG is seeded from `params.seed` (397), the count comes from `params.count` (389-390), and pscale takes no part in sampling.
- So the point count, order and positions come out identical. The normals are recomputed identically (484-490).
- The only difference is `pscale.set<float>(point, params.pscale)` (492), on a point attribute added every cook (449-452).
- The output holds only the scattered points: no primitives, and no `path` or `name`.

**2. GeoUsdConvert maps pscale to `widths` on one UsdGeomPoints prim.**
- The points have no primitives, so the result is one UsdGeomPoints partition at `/geo/points_0`. The kind and path are unchanged step to step.
- `pscale` becomes `widths` with vertex interpolation (`GeoUsdConvert.cpp:1241-1246`; point class maps to vertex at 1162-1165).
- `extent` is computed from the positions only (`extentOf(points)`, 1822), so it does not change.
- There is no PointInstancer, so nothing in the instancer path is involved.

**3. `GeoLayers::refresh` makes one info-only edit: no erase, no re-author, no resync.**
- The partition signature changes, because the pscale pages differ (`computeSignature` 1611+). So `kept` is 0.
- `topologyChanged` is false: there are no face or curve counts, and the `points` array size is the same (`GeoSdfFileFormat.cpp:437-455`). So nothing is rebuilt.
- The field diff (713-735) compares every field:
  - `points` and `normals` are equal in value but sit in different buffers, so each costs a full O(N) compare and gets no SetField;
  - `extent` is equal;
  - only `widths` gets `SetField`.
- That is an info-only change on `/geo/points_0.widths`. UsdImaging dirties the widths primvar **(USD, unverified)**.
- No prim is removed or added, so SceneDrawFilter admission is never involved: nothing queues (`_PrimsAdded` 1265+ never runs). `_PrimsDirtied` passes the dirty through (1366-1394).

**4. The guides add nothing visible.**
- The detail changed, so `editStage` rebuilds the packed-bounds and selection guides (`Gui/GeoDisplay.cpp:793-940`).
- With no packed primitives and no selection those are `removeGuide` no-ops.
- Markers stay off unless point markers or points pick mode are on.

**5. The frame path has nothing that presents grey for this edit.**
- The stack is the same, so the frame is kept and `composesInPlace` holds (`SceneViewport.cpp:10967`, 9668).
- No error, no empty payload and no stage hold apply, so `blank` is false (12788).
- The camera does not move, so there are no stand-ins (8879-8888).
- Frames go through the double-buffered pacer (sections 2.G and 2.K above).

### So where does the grey come from? Ranked

**1. The drawn content itself (most likely; not a pipeline bug).**
- Widths are pscale, taken as the point's full diameter in world units (1242-1243).
- The knob's display range goes up to 1 (`GeoScatter.cpp:594-596`). Its default is 0.01, which its tooltip says "suits points on geometry about a unit across".
- 1000 points (the default count, 565) on a unit Box at pscale 0.2 to 1 are sprites many times their spacing. They merge into one solid mass.
- That mass is shaded in Storm's fallback displayColor, which is mid grey **(USD fallback 0.5, unverified)**, over the viewer's dark-grey background (`kBackgroundSrgb = 0.2478`, `SceneViewport.cpp:213`).
- While the slider passes through the upper half of its range, the view reads as "going grey". It comes back as the value drops. Wiggling the slider makes it look like flashing.
- `extent` ignores widths (1822), which UsdGeomPoints' own ComputeExtent does not do. Large sprites near the frame edge can therefore be frustum-culled and pop in and out. That is a minor bug of its own.

**2. Something outside our frames (needs measuring).** The candidates are Qt composition, the non-threaded-GL fallback (`renderIfIdle`), or the jump-back race (section 2.A). The race is invisible here, because each step's positions are identical. Only the sprite size would jump back.

**3. A failed cook (section 2.B).** It would also show red error text, which the user did not report.

### Checks that settle it
1. Stop the drag at a high value (say 0.5). Does the view stay grey at rest? If so it is (1). Compare it with screenshots at 0.01 and 0.1.
2. Turn on Transfer Cd on a coloured source, or put a GeoWrangle setting Cd after the scatter. If the "grey" turns into the source colour, it is (1).
3. If the grey appears only mid-drag and the same value looks fine at rest, log the paintGL branch per paint and use the presented-frame coverage detector (section 5.2).

### Fix ideas, if (1) is confirmed
- Lower the display maximum, or label pscale as a radius (widths = 2·pscale is the common pscale-as-radius convention; check the reference DCC docs first).
- Pad the extent by half the maximum width.
- Treat the cost as cosmetic: about 3 O(N) compares per step for an edit that changes one float array.
