# M510 - MoonRay Warning Text

`MoonRayAovs::warningText` (`Engine/Nodes/Scene/MoonRayAovs.cpp:608`) turns any MoonRay warning or error whose text starts with a prim path (`leadingPath`) into "MoonRay could not render these <kind> as authored: <paths>" (:664) and drops MoonRay's own message. A frame-held USDProject3D applied to two cards through USDAssignMaterial raises this warning even though the render is correct. Capture the real notice for that scene, show MoonRay's text in the warning, and filter the notice if it is benign.

## Scope

**In:**
- Capturing the notice from a two-card, frame-held projection scene.
- Keeping MoonRay's message per prim in the warning.
- Filtering the notices that P1 shows are benign.
- Regression tests in `Tests/Scene/MoonRayAovs_Test.cpp` and the scene test.

**Out:**
- Cycles' warning path (`DelegateSupport::warningText`).
- `errorText` grouping (:673), unless P1 shows the notice arrives as an error.
- MoonRay test flakiness (M496 - MoonRay Test Stability).

**Edge cases:**
- Several notices for one prim: list each distinct message once.
- A benign filter must match MoonRay's exact wording, never a prefix that could hide a real failure.

**Assumptions:**
- The notice comes through `MoonRayAovs::LogCapture` (MoonRayAovs.h ~:118) at warning level or above.

## Phase 510.1: Capture

- [x] M510.P1.T1 — Scene test that renders two cards under a frame-held projection and logs the notices
  - files: `Tests/Scene/USDRenderMoonRay_Test.cpp` (the `USDRenderMoonRayProjectionTest` fixture, ~:1833)
  - approach: extend the fixture graph with a second card. Assign the frame-held projection material (`kUSDProject3DParamUseFrameHold`, `kUSDProject3DParamFrameHold`) to both through `USDAssignMaterial`, render one frame, and write every captured notice (level and text) and the node's warning to stderr. For now, assert only that the render succeeds and both cards show the plate.
  - verify: PM runs it at the phase build and records the exact notice text, and whether it is benign, as a milestone decision
  - size: M

## Phase 510.2: Fix

- [x] M510.P2.T1 — Show MoonRay's message and filter the benign notice
  - files: `Engine/Nodes/Scene/MoonRayAovs.cpp`, `Engine/Nodes/Scene/MoonRayAovs.h`, `Tests/Scene/MoonRayAovs_Test.cpp` (update `WarnsOnePerKindAndKeepsOtherErrorsForTheNode`, :163)
  - approach: keep one line per kind, but name each prim with MoonRay's remaining text (`rest` from `leadingPath`), e.g. `/World/Card1 (<MoonRay's text>)`, deduplicated. Add a small exact-match filter for the notices P1.T1 recorded as benign, with a comment saying why each is harmless. Never mention other products in the comments.
  - verify: `ctest -R MoonRayAovs` passes at the phase build
  - size: M
- [x] M510.P2.T2 — Regression tests for the captured notice
  - files: `Tests/Scene/MoonRayAovs_Test.cpp`, `Tests/Scene/USDRenderMoonRay_Test.cpp`
  - approach: in `MoonRayAovs_Test`, the captured notice text gives no warning (if benign) or a warning carrying MoonRay's text, and an unknown prim warning keeps its text. In the P1.T1 scene test, assert the node's warning is empty (or carries MoonRay's text) and drop the stderr dump. Bundled gtest is old: no `GTEST_SKIP`, `INSTANTIATE_TEST_CASE_P` only, no `ASSERT_TRUE(ptr)`.
  - verify: `ctest -R "MoonRayAovs|USDRenderMoonRay"` passes at the phase build
  - size: M

**Verification gate:** `ctest -R "MoonRayAovs|USDRenderMoonRay"` green; full debug PR CI green.

## Decisions
- 2026-11-22 — P1.T1 recorded (gate on cfde0f2ed): a frame-held projection on two cards makes hdMoonray emit, per card, level-2 notices `<prim>.primvars:natronProjectionFrameHold1: GfMatrix4d not translated` and `<prim>.primvars:natronProjectionFrameHold1Column{0,1,2}: VtArray<GfVec4f> not translated`; the render still projects correctly on both cards (EXPECT_PROJECTS passes). They are benign: Natron's own frame-hold primvars, which hdMoonray can't translate and doesn't need (the projection reads the column primvars through the shader). The filter drops exactly these — primvar name `natronProjectionFrameHold<N>` or `natronProjectionFrameHold<N>Column<0-2>` with "not translated" — matched in full, not by prefix.
