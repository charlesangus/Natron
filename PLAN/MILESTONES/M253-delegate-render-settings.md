# M253 - Delegate Render Settings

Expose each render delegate's settings as knobs on USDRender: a curated group for Cycles and
one for MoonRay, plus a generated "Other settings" group that makes every setting the
delegate declares reachable (user, 2026-10-27; see "A port means every control"). Follows
M251 - Cycles Rendering and M252 - MoonRay Rendering.

## Scope

**In:**
- A generic builder that turns `HdRenderSettingDescriptorList` (name, key, default) into knobs of the matching type, rebuilt when the `Renderer` knob changes.
- Curated knob groups: Cycles (samples, max bounces and per-type bounces, clamping, pixel filter, seed, denoise, light-tree) and MoonRay (pixel samples, light samples, bsdf samples, max depths, pixel filter, seed, denoise). Labels are readable forms of the script names (matrixSize → "Matrix Size").
- Settings persist with the project, are hashed into the node's render hash and are passed to the delegate before each render.
- The viewer's renderer uses the same settings group, held in a viewer-local copy (a separate max-samples for the viewer arrives in M254 - Progressive Viewer Render).

**Out:**
- Progressive stop criteria — M254 - Progressive Viewer Render.
- AOV/LPE selection — M256 - Delegate AOV Selection.
- GPU device settings — none.

**Edge cases:**
- Switching renderer hides the old group; values are kept so switching back restores them.
- A setting the installed delegate version no longer declares: its knob disappears; the stored value stays in the file and is reported once.
- A project saved with a delegate that is not installed keeps its group's values untouched.
- Denoise settings that need models or libraries the build lacks: the knob is disabled with a tooltip, not hidden.

**Assumptions:**
- The delegates describe their settings through `GetRenderSettingDescriptors`; Cycles' list may be shorter than its UI. Verify by dumping both lists in the first task; any curated control missing from a delegate's list is reported rather than invented.

## Phase 253.1: Generic and curated settings

- [ ] M253.P1.T1 — Generic descriptor-to-knob builder
  - files: `Engine/Nodes/Scene/RenderSettingsKnobs.h`, `Engine/Nodes/Scene/RenderSettingsKnobs.cpp`, `Engine/Nodes/Scene/USDRender.cpp`, `Tests/Scene/RenderSettingsKnobs_Test.cpp`
  - approach: map VtValue types (bool, int, float, string, token, GfVec3f/4f colours) to Natron knobs; label from the key; group under "Other Settings"; rebuild on `Renderer` change. Include a dump command used by the next tasks to print each delegate's list.
  - verify: unit test builds knobs from a synthetic descriptor list of every supported type; round-trips values through save/load.
  - size: L

- [ ] M253.P1.T2 — Pass settings to the engine and hash them
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/USDRender.cpp`
  - approach: before each render call `SetRendererSetting` for every changed value; include all setting values in the render hash.
  - verify: changing a setting changes the hash and the next render uses it (a samples-count test via the delegate's reported sample count).
  - size: M

- [ ] M253.P1.T3 — Curated Cycles group
  - files: `Engine/Nodes/Scene/CyclesSettings.cpp`, `Engine/Nodes/Scene/CyclesSettings.h`
  - approach: using the dump from T1, create labelled knobs for the controls listed in Scope, bound to the delegate's keys; remove them from the generated group so none appears twice.
  - verify: each curated knob changes the delegate's setting (test reads it back); a params → knob audit lists every Cycles descriptor and the knob that reaches it, with none unreachable.
  - size: M

- [ ] M253.P1.T4 — Curated MoonRay group
  - files: `Engine/Nodes/Scene/MoonRaySettings.cpp`, `Engine/Nodes/Scene/MoonRaySettings.h`
  - approach: as the previous task for MoonRay's controls.
  - verify: each curated knob changes the delegate's setting; the params → knob audit passes.
  - size: M

**Verification gate:** CI green; every setting either delegate declares is reachable through a curated or a generated knob (audit output attached to the PR); settings persist and drive renders; a Xvfb screenshot of both property panels is shared with the user.

## Decisions

- 2026-10-27 — **Curated plus generic fallback (user):** hand-built groups for the two shipped delegates, generated knobs for everything else.
