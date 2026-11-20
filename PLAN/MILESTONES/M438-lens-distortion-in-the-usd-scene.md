# M438 - Lens Distortion In The USD Scene

A research spike: can a camera's lens distortion travel in the USD scene alongside the camera, so nodes that take a camera get its distortion with it? The nodes in question are CardProject25D, USDRender, USDProject3D and USDProjectUV. The distortion need not sit on the camera prim itself (user, 2026-11-14). It may be any scene representation associated with the camera: a related prim, an applied schema, a relationship, or custom attributes. The spike covers both forms, parametric lens models and image maps (STMap/IDistort), and recommends one or both. The user chooses before any implementation milestone is filed.

## Scope

**In:**
- Research what exists before designing: OpenUSD's own camera and lens schemas and any lens-distortion proposals in the pxr source/docs in `.local/docs`, Houdini Solaris, Katana, Gaffer and Blender (`.local/docs`). Check what the bundled delegates (Storm, Cycles, MoonRay, Nucleon Utility) can consume natively.
- Two candidate encodings, worked out far enough to compare:
  - parametric: our `LensModels.h` model types and parameters, animatable;
  - maps: asset paths, or M233 - Live Node Textures references to STMap/IDistort images, plus their conventions.
- How a node finds a camera's distortion, how it round-trips through USDWrite and USDRead, and how M132 - Native Lens Distortion's nodes could author it.
- A written design and recommendation, with sketches of the follow-up milestones.

**Out:**
- Any production code: the follow-up implementation milestone is filed by `/cat-plan` once the user picks.
- Changing CardProject25D's map inputs: they stay in either case (M437 - CardProject25D).

**Edge cases:**
- A camera with several lens setups (shot versus plate lens): the design says how a node chooses one.
- Distortion animated over time (focus breathing, zoom): both encodings must say how they animate.

## Phase 438.1: Research and design

- [x] M438.P1.T1 — Prior-art survey
  - files: write `PLAN/DESIGN/2026-11-14-usd-lens-distortion-research.md` (plan worktree)
  - approach: grep the pxr headers and source in the dev image (`UsdGeomCamera`, any `Lens`/`Distortion` schema) and the `.local/docs` trees (USD, Solaris, Katana, Gaffer, Blender). Record what each offers for lens distortion in a scene, with file or page references, plus what Cycles and MoonRay can consume.
  - verify: the doc cites a source for each claim; the user is pointed at it.
  - size: M

- [x] M438.P1.T2 — Design and recommendation
  - files: write `PLAN/DESIGN/2026-11-14-usd-lens-distortion-design.md`
  - approach: compare parametric and map encodings on exactness, animation, resolution independence, delegate support, authoring from LensDistortion, and USDWrite round-trip. Recommend one or both, with a schema sketch (prim, attributes, relationships), the lookup rule nodes use, and outline milestones. Put the choice to the user as an open question.
  - verify: the user has the doc and the question is on the board's `# Open questions`.
  - size: L

**Verification gate:** both design docs exist on the plan branch, the recommendation has been put to the user, and the user's choice is recorded as a decision. No code ships, so there is no PR. The milestone is `done` once the choice is recorded and the follow-up milestones are filed.

## Decisions

- 2026-11-14 — Distortion may live anywhere in the scene associated with the camera, not literally on the camera prim (user).
- 2026-11-14 — The spike covers both parametric models and maps, and recommends (user, grill).
- 2026-11-20 — Research and design done: PLAN/DESIGN/2026-11-14-usd-lens-distortion-research.md and -design.md. No OpenUSD scene schema carries distortion, and usdImaging drops `lensDistortion:*` camera attributes before Hydra's HdLensDistortionSchema (probe in OpenUSD 26.08); no bundled delegate reads that schema. **PM call, by the match-the-reference-DCC default:** the scene carries both encodings in one lens prim type, parametric first and maps for lenses no model covers. It is a child of the camera by default, picked through an ordered `natron:lenses` relationship, and each node gets a Lens dropdown next to its camera dropdown. The reference DCC stores distortion on a separate prim bound to the camera by a relationship, parametric or texture map. The user can override. The follow-up milestones A–G in the design doc go to /cat-plan.
- 2026-11-20 — **Both encodings, in a lens prim (user):** the user confirmed the design doc's recommendation. Remaining for done: file the follow-up milestones A–G from the design doc (/cat-plan; see Open questions).
