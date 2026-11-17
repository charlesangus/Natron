# M258 - MoonRay Deep Output

The Natron side of MoonRay deep: USDRender with Renderer = MoonRay requests deep and outputs a DeepImage. MoonRay stays out of process. M460 - MoonRay True Deep Patch makes MoonRay record every surface and volume segment along the camera path, M461 - MoonRay DeepFrame Over Arras carries the resolved deep buffer over Arras in bands into the `HydraDeepBuffer` M286 - Utility Renderer Deep Output defines, and this milestone wires it into USDRender.

Drafted by M255 - Delegate Output Spikes (`spike/m255/REPORT-P2T3-drafts.md`, a1e0d29fd, branch `spike/m255-delegate-output`); phases 258.1 and 258.2 of that draft became M460 and M461 (user, 2026-11-16).

## Scope

**In:**
- USDRender's deep request on MoonRay: `deep_z_tolerance` from Merge Distance, the opacity-only channel set, and the host `DeepCompaction`.
- Deep comp parity, opacity parity and cancel tests, and the USDRender docs note.

**Out:**
- The MoonRay fork patches: M460 - MoonRay True Deep Patch.
- The DeepFrame message, banded send and hdMoonray assembly: M461 - MoonRay DeepFrame Over Arras.
- MoonRay's DCX writer: not fixed (user, 2026-11-16); it is batch-only and outside our path.
- Deep for progressive passes and multi-machine merge (`progmcrt_merge`): final frame only.
- Denoised deep: never.

**Edge cases:**
- Deep Content = Opacity Only: the request asks for A/Z/ZBack only, and the bands omit colour and AOVs.
- Abort during the deep transfer: returns within 1 s.
- Test scenes use presence (`DwaBaseMaterial`), not `UsdPreviewSurface` opacity, which MoonRay doesn't treat as presence.

**Assumptions:**
- Tearing down the MoonRay Hydra delegate after an Arras render can crash (M255 risk); the tests here keep one delegate alive per test, as the existing MoonRay tests do.

## Phase 258.3: Natron side

- [ ] M258.P3.T1 — USDRender deep on MoonRay
  - files: `Engine/Nodes/Scene/MoonRayAovs.{h,cpp}`, `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Tests/Scene/USDRenderMoonRay_Test.cpp`
  - approach: needs M286.P2.T3 and P2.T4. Bind the `deep` AOV with `deep_z_tolerance` = USDRender's Merge Distance; Opacity Only requests the opacity-only channel set; run the host `DeepCompaction` on the assembled buffer.
  - verify: gtest: deep has R G B A Z ZBack; Opacity Only gives exactly A/Z/ZBack with alpha equal to Full's within 1e-4; a DeepMerge holdout of a card behind a 50% presence card matches flat; abort during transfer returns within 1 s.
  - size: M

- [ ] M258.P3.T2 — Fog in a deep comp and docs
  - files: `Tests/Scene/USDRenderMoonRay_Test.cpp`, `Engine/Nodes/Scene/docs/USDRender.md`
  - approach: USDRender (MoonRay, deep) of a fog box behind a presence card → DeepMerge with a DeepFromImage card → DeepToImage, compared with flat. Document MoonRay deep: final frame only, never denoised.
  - verify: gtest: segments have zback > z; the comp matches flat within tolerance.
  - size: M

**Verification gate:** build and ctest green on the MoonRay image carrying M460 and M461; under Xvfb, a MoonRay deep render of presence cards and fog is merged with a card and flattened, with screenshots shared with the user (collected at F24 - Deep Rendering's UAT); band count and bytes per frame logged at 1080p.

## Decisions

- 2026-11-05 — Volume segments are required in MoonRay deep output (user): smoke must hold out correctly in a deep comp. M255 - Delegate Output Spikes has to show the transport can carry them; if it can't, bring that back to the user rather than dropping this.
- 2026-11-16 — MoonRay deep travels over Arras, MoonRay stays out of process (user): a banded DeepFrame Arras message after the FINISHED frame, received by hdMoonray into `HydraDeepBuffer`; deep is final-frame only and multi-machine merge (`progmcrt_merge`) gets no deep. Rejected: in-process rendering for deep (loses crash/memory isolation and concurrent renders; one global RenderDriver; process-wide TBB cap), a deep EXR side output read back from disk (half-float AOVs, temp files, delayed cancel), and a separate batch render (double cost, deep would not match the flat AOVs). Full record: PLAN/DECISIONS/2026-11-16-moonray-deep-over-arras.md.
- 2026-11-16 — **Deep opacity output (user, inbox):** MoonRay honours the opacity-only channel set, and the DeepFrame bands omit colour and AOVs when it is set. See `PLAN/DECISIONS/2026-11-16-deep-opacity-output.md`.
- 2026-11-16 — **Elaborated from M255 - Delegate Output Spikes' drafts and split in three (user, inbox and grill):** the fork patch is M460 - MoonRay True Deep Patch and the Arras transport is M461 - MoonRay DeepFrame Over Arras, so fork work can run alongside M286 - Utility Renderer Deep Output. Volume segments carry colour and alpha; DCX is not fixed; compaction applies to every delegate (user, M255 P2.T3 decisions 5–7).
