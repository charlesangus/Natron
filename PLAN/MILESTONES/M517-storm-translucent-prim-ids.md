# M517 - Storm Translucent Prim Ids

Make USDRender's primId plane, and the picking that reads it, see translucent prims under Storm. Storm draws the translucent material tag through its OIT passes, which write colour only, so every prim with a translucent material is missing from primId. That includes every card carrying a projection, because `Engine/Nodes/Scene/shaders/natronProject3D.glslfx` is tagged translucent (found in M281 - Utility Materials And Lights). The fix writes ids for translucent prims in a pass of our own after the engine's render, rather than patching Storm: the image's OpenUSD comes from the ASWF base with no source patches (`tools/ci/local/Dockerfile`). USDRender runs Storm through `UsdImagingGLEngine` (`Engine/Nodes/Scene/HydraRenderImpl.cpp:469`); the 3D viewer's selection pass already builds per-material-tag tasks of its own (`Gui/SceneSelectionTasks.cpp:248-370`), which is the pattern to follow.

## Scope

**In:**
- The primId plane of a Storm render holds the nearest drawn translucent fragment in front of the opaque surface, and the opaque id elsewhere.
- Every translucent prim, not only projected cards.
- Consumers of the plane (`HydraRenderImpl.cpp:1893`, the ids output and picking on USDRender's result) need no change.

**Out:**
- Changing the projection material's tag: none (user chose ids in the OIT path, 2026-11-22).
- The depth plane: keeps Storm's behaviour.
- Cycles and MoonRay: their primId already comes from the delegate.
- Patching OpenUSD's source: none without asking the user.

**Edge cases:**
- A fragment the material discards writes no id, so outside a projection's data window the card stays unpicked, as now.
- Semi-transparent fragments write their id, matching Storm's pick, which applies no alpha threshold to translucent materials.
- A render with no translucent prims, or that doesn't request primId, skips the pass and stays byte-identical.

**Assumptions:**
- The image's `UsdImagingGLEngine` (OpenUSD 0.26.08) lets a subclass reach its render index and run extra tasks (its protected `_GetRenderIndex` and `_Execute`). M517.P1.T1 checks the header first.

## Phase 517.1: Id Pass

- [ ] M517.P1.T1 — Write translucent prim ids after the Storm render
  - files: `Engine/Nodes/Scene/StormIdPass.{h,cpp}`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/CMakeLists.txt`, `Tests/Scene/StormTranslucentIds_Test.cpp`
  - approach: first read the image's `pxr/usdImaging/usdImagingGL/engine.h`; if a subclass can't reach the render index and execute a task, stop and report the options to the user. Otherwise wrap the engine in a subclass and, after `Render`, execute one `HdxRenderTask` on the translucent material-tag collection that writes the engine's primId (and instanceId) AOV buffers, with blending off, depth test against the engine's depth, and depth write off, so the nearest drawn translucent fragment wins. Follow `Gui/SceneSelectionTasks.cpp`'s task setup. Run it only when primId is requested and the collection is non-empty.
  - verify: gtest — a projected card in front of an opaque plane: primId at the card's pixels decodes to the card's path and elsewhere to the plane's; a pixel outside the projection's data window decodes to the plane; a scene with no translucent prims gives a primId plane byte-identical to the same render with the pass disabled.
  - size: L

- [x] M517.P1.T2 (dropped, user 2026-11-22) — Check picking on projected cards
  - files: `Tests/gui/storm_translucent_pick.py`, `Tests/fixtures/storm_translucent_pick/`
  - approach: a GUI script under Xvfb loads a small scene with a USDProject3D card over a ground plane rendered by USDRender with Storm, and picks on USDRender's output and in the 3D viewer at a card pixel and a plane pixel.
  - verify: the script passes under Xvfb: the card pixel selects the card, the plane pixel the plane, in both places.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `StormTranslucentIds_Test` and the existing HydraRender and projection tests pass; `storm_translucent_pick.py` passes under Xvfb.

## Decisions

- 2026-11-22 — **Ids for translucent prims, not an opaque projection material (user):** chosen over tagging the projection material opaque when its plate is opaque, so semi-transparent cards and every other translucent prim pick too. Written in our own pass because the base image's OpenUSD carries no patches (cat-plan); patching Storm needs the user's say.
- 2026-11-22 — P1.T2 (GUI picking check) dropped (user, 2026-11-22): nothing picks from USDRender's 2D output, and the gtests prove the id planes.
- 2026-11-22 — Translucent ids use a private depth buffer (implementer): opaque/masked prims redraw depth into it, then translucent prims draw ids with depth test and write on, so the nearest translucent id wins; Storm's own colour and depth are untouched. Pending the user's confirmation (asked whether draw order would do; it would not — see Open questions).
- 2026-11-23 — **Private id depth buffer approved (user):** translucent ids use the private depth buffer (nearest wins), as implemented in 0bb3f6fbd.
- 2026-11-23 — Merged to main as 8005b91a1 (#354); review round closed.
