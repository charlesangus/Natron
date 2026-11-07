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

- [x] M253.P1.T1 — Generic descriptor-to-knob builder
  - files: `Engine/Nodes/Scene/RenderSettingsKnobs.h`, `Engine/Nodes/Scene/RenderSettingsKnobs.cpp`, `Engine/Nodes/Scene/USDRender.cpp`, `Tests/Scene/RenderSettingsKnobs_Test.cpp`
  - approach: map VtValue types (bool, int, float, string, token, GfVec3f/4f colours) to Natron knobs; label from the key; group under "Other Settings"; rebuild on `Renderer` change. Include a dump command used by the next tasks to print each delegate's list.
  - verify: unit test builds knobs from a synthetic descriptor list of every supported type; round-trips values through save/load.
  - size: L

- [x] M253.P1.T2 — Pass settings to the engine and hash them
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`, `Engine/Nodes/Scene/USDRender.cpp`
  - approach: before each render call `SetRendererSetting` for every changed value; include all setting values in the render hash.
  - verify: changing a setting changes the hash and the next render uses it (a samples-count test via the delegate's reported sample count).
  - size: M

- [x] M253.P1.T3 — Curated Cycles group
  - files: `Engine/Nodes/Scene/CyclesSettings.cpp`, `Engine/Nodes/Scene/CyclesSettings.h`
  - approach: using the dump from T1, create labelled knobs for the controls listed in Scope, bound to the delegate's keys; remove them from the generated group so none appears twice.
  - verify: each curated knob changes the delegate's setting (test reads it back); a params → knob audit lists every Cycles descriptor and the knob that reaches it, with none unreachable.
  - size: M

- [x] M253.P1.T4 — Curated MoonRay group
  - files: `Engine/Nodes/Scene/MoonRaySettings.cpp`, `Engine/Nodes/Scene/MoonRaySettings.h`
  - approach: as the previous task for MoonRay's controls.
  - verify: each curated knob changes the delegate's setting; the params → knob audit passes.
  - size: M


## Phase 253.4: Organised Render Settings

The user asked for the render settings panel to be organised and easier to understand (2026-11-05, "surely eg cycles in blender isn't just a flat list"). The flat curated group becomes table-driven named sections, modelled on Blender's Cycles panels: tab, section, at most one subsection. Dependents grey out, raw enum ints become menus, and tooltips state the trade-off. Every setting keeps its script name and nothing is removed.

**Layout.** `[open]`/`[closed]` is the default state, `+` a knob generated today, `~` a knob greyed by the rule in brackets. Knobs are shown without their prefix.

Cycles (`cycles<Group>` script names):
- Sampling [open]: Samples, +Use Adaptive Sampling, ~+Adaptive Threshold, ~+Adaptive Min Samples [adaptive], +Time Limit, Seed.
- Denoise [open]: Use Denoise, Denoiser Type (always off), greyed by [Use Denoise]: ~+Denoiser Quality, ~+Denoiser Prefilter, ~+Denoiser Passes, ~+Denoise Start Sample, ~+Denoiser Upscale Factor. +Denoise Use GPU stays always off.
- Light Paths [open]:
  - Bounces [open]: Max Bounce, greyed by [Max Bounce > 0]: ~Max Diffuse Bounce, ~Max Glossy Bounce, ~Max Transmission Bounce, ~Max Volume Bounce. Transparent Max Bounce is not greyed.
  - Clamping [open]: Sample Clamp Direct, Sample Clamp Indirect.
  - Caustics [closed]: +Caustics Reflective and +Caustics Refractive on one row, +Filter Glossy.
  - Fast GI [closed]: +AO Bounces, +AO Factor, +AO Distance, +AO Additive Factor.
- Lights [closed]: Use Light Tree, ~+Light Sampling Threshold [light tree off].
- Volumes [closed]: +Volume Ray Marching, ~+Volume Max Steps, ~+Volume Step Rate (T3 confirms the grey polarity from source).
- Path Guiding [closed]: +Use Guiding, greyed by it: ~+Deterministic Guiding, ~+Use Surface Guiding, ~+Surface Guiding Probability [also surface], ~+Use Volume Guiding, ~+Volume Guiding Probability [also volume], ~+Guiding Training Samples, ~+Use Guiding Direct Light, ~+Use Guiding MIS Weights, ~+Guiding Distribution Type, ~+Guiding Directional Sampling Type, ~+Guiding Roughness Threshold.
- Light Contributions [closed]: +Use Direct Light, +Use Indirect Light, +Use Diffuse, +Use Glossy, +Use Transmission, +Use Emission.
- Advanced [closed]: +Sampling Pattern (menu), +Scrambling Distance, +AA Samples, +Min Bounce, +Transparent Min Bounce, +Motion Blur, +Use Pixel Jitter, +Use Custom Pixel Jitter Sample, ~+Custom Pixel Jitter Sample.
  - Sample Subset [closed]: +Sample Offset, +Use Sample Subset, ~+Sample Subset Offset, ~+Sample Subset Length.
- Other Settings [closed]: only for keys no table entry claims.

MoonRay (`moonraySampling`, `moonrayDepth`, ...; `moonrayLogging` avoids colliding with the `moonrayDebug` and `moonrayInfo` knobs):
- Sampling [open]: Pixel Samples, Light Samples, BSDF Samples, BSSRDF Samples.
- Depth [open]: Max Depth, greyed by [Max Depth > 0]: ~Max Diffuse Depth, ~Max Glossy Depth, ~Max Mirror Depth. Max Volume Depth stays live.
- Filtering [open]: Pixel Filter, Pixel Filter Width.
- Denoise [open]: Enable Denoise, Enable OIDN (always off, follows; after its leader), ~Denoise Albedo Guiding and ~Denoise Normal Guiding on one row.
- Scene Toggles [closed]: +Enable Motion Blur, +Disable Lighting, +Double Sided, +Decode Normals, +Force Polygon.
  - Prune [closed]: +Prune Willow, +Prune Fur Deform, +Prune Volume, +Prune Wrap Deform, +Prune Curve Deform.
- Performance [closed]: +Execution Mode (menu auto/vectorized/xpu/scalar, `ArrasSettings.cc:266`), +Max Fps.
- Remote [closed]: +Use Remote Hosts, +Remote Hosts (both always off, keeping their reason).
- Logging [closed]: +Log Level, +Debug, +Info, +Rdl Output.
- Other Settings [closed]: leftovers. `localReservedCores` and `maxConnectRetries` stay excluded (`MoonRaySettings.cpp:129-130`).

Storm and generated renderers, through the classifier: Storm gets Volumes (Volume Raymarching Step Size, Volume Raymarching Step Size Lighting, Volume Max Texture Memory Per Field), Lights (Max Lights), Performance (Enable Tiny Prim Culling), all open. For a generated renderer all groups open when it has 20 or fewer generated knobs, otherwise only the first. Other Settings is always closed and last.

**Classifier.** A renderer's exact key table wins. Otherwise match token prefixes of the key's last segment, split on `_` and camel case, first rule wins: (1) denois, oidn → Denoise; (2) guiding → Path Guiding; (3) volume, raymarch → Volumes; (4) bounce, depth → Bounces; (5) clamp → Clamping; (6) light, shadow → Lights; (7) sample, sampling, seed, adaptive, noise → Sampling; (8) filter → Filtering; (9) motion, shutter, blur → Motion; (10) thread, memory, cache, cull, cores, fps, execution, tile, bucket → Performance; (11) log, debug, verbose → Logging; none → Other Settings. Empty groups are never created.

**Constraints.** Groups are unhashed and the digest walks the flat knob list (`Engine/Node.cpp:980-1058`), and `values()`/`author()` walk `_imp->settings`, so moving knobs under groups changes neither. Grey with knob-level enable: not group disable (`KnobGuiGroup::setEnabled` keeps its own restore list) and not secret. `setAddNewLine(false)` for bool pairs only. Indent is a constant 12 px, so no third level. Add `GI` to the label acronym list. No tooltip mentions Nuke. Layouts fit 1920x1080 scale 1. This phase adds no new controls.

- [x] M253.P4.T1 — Add table-driven nested groups to the renderer pages
  - files: `Engine/Nodes/Scene/RenderSettingsKnobs.{h,cpp}`, `Tests/Scene/RenderSettingsKnobs_Test.cpp`
  - approach: Add `Curated::groups` (`GroupSpec{name, parent, open, tooltip}`) and `CuratedSetting::group`. Create groups lazily in table order, labels from `labelFromScriptName(name, prefix)`. Reject depth above 2. A group name equal to a setting's natural script name fails a test rather than using the fallback at `:1029-1034`. A setting with no group goes to the existing curated group, so Cycles and MoonRay are unchanged until T3 and T4.
  - verify: with a test table via `setDescriptorsForTests`, parents and order match, empty groups are absent, open states match; namespace digests are equal for the same values under two layouts; toggling a group leaves the digest and `values()` unchanged.
  - size: M
- [x] M253.P4.T2 — Classify generated settings into groups and apply it to Storm
  - files: `Engine/Nodes/Scene/RenderSettingsKnobs.cpp`, `Tests/Scene/RenderSettingsKnobs_Test.cpp`, `Tests/Scene/USDRenderSettings_Test.cpp`
  - approach: Add the classifier above for keys without a curated entry, with the 20-knob open rule. Unmatched keys go to `<prefix>OtherSettings`.
  - verify: a table test covers each rule, the rule-order cases and leftovers; `:325` keeps an unmatched key in its fixture; `audit` still reports every key with a knob; Storm's real keys land as laid out (its key names are unconfirmed, check them).
  - size: M
- [x] M253.P4.T3 — Lay out the Cycles tab and turn its enums into menus
  - files: `Engine/Nodes/Scene/CyclesSettings.{h,cpp}`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: Promote every claimed socket to a `CuratedSetting` with today's script name and `group`. Add int-valued menus for Denoiser Prefilter and Quality (`src/device/denoise.h:38-58`), Sampling Pattern (`integrator.cpp:148-153`), Direct Light Sampling Type and both guiding types (`src/kernel/types.h`). Denoiser Passes stays an int bitmask. No new `setMinimum` caps. The `cyclesSettings` group knob goes away; tests at `:767` change.
  - verify: the audit at `USDRenderCycles_Test.cpp:813-822` has no gaps, every knob has the expected parent, each menu's default resolves to an entry, script names match a list captured before the change. Add an explicit skip list if the nudge loop at `:763-775` is unsuitable for a promoted key.
  - size: L
- [x] M253.P4.T4 — Lay out the MoonRay tab
  - files: `Engine/Nodes/Scene/MoonRaySettings.{h,cpp}`, `Engine/Nodes/Scene/RenderSettingsKnobs.{h,cpp}`, `Tests/Scene/USDRenderMoonRay_Test.cpp`
  - approach: The same promotion. Move the two `disabledKeys` entries into Remote with their reasons, add the Execution Mode string menu, add `Curated::pageLabel` so the tab reads "MoonRay" rather than "Moonray" (`RenderSettingsKnobs.cpp:1002-1004`). Keep the Enable Denoise leader before Enable OIDN. The `moonraySettings` group knob goes away; tests at `:815` change.
  - verify: the audit at `USDRenderMoonRay_Test.cpp:862-871` shows no gaps and exclusions unchanged; parents and tab label match; `follows` still syncs.
  - size: M
- [x] M253.P4.T5 — Grey dependent knobs from their switches
  - files: `Engine/Nodes/Scene/RenderSettingsKnobs.{h,cpp}`, `Engine/Nodes/Scene/USDRenderSettings.cpp`, `Engine/Nodes/Scene/CyclesSettings.cpp`, `Engine/Nodes/Scene/MoonRaySettings.cpp`, `Tests/Scene/RenderSettingsKnobs_Test.cpp`
  - approach: Add `enabledBy` rules (key plus inverted flag, all must hold) and `refreshEnabled()`, called from `knobChanged` (`RenderSettingsKnobs.cpp:1144-1155`), after `addRenderer`, and from `onKnobsLoaded`. Saved per-dimension `enabled` state (`Serialization/KnobSerialization.h:96`) must not win over the computed one. `disabledReason` knobs always stay off. An expression driving a switch does not re-grey dependents; accepted, since values still render correctly.
  - verify: toggling a switch greys and restores dependents; AND rules work; a `disabledReason` knob never turns on; state is correct after save and load; `values()` and the digest are identical with enablement toggled and values fixed.
  - size: M
- [x] M253.P4.T6 — Rewrite tooltips, same-row pairs and docs
  - files: `Engine/Nodes/Scene/CyclesSettings.cpp`, `Engine/Nodes/Scene/MoonRaySettings.cpp`, `Engine/Nodes/Scene/docs/USDRenderSettings.md`, `Tests/Scene/USDRenderSettings_Test.cpp`
  - approach: Each curated setting gets its effect, trade-off and sensible values, read from Cycles and scene_rdl2 source rather than memory, keeping the "Authored as ..." suffix. Example: "Limits the brightness of indirect samples. Lower values remove fireflies but lose energy; 10 is a good start, 0 turns it off." Add group tooltips and the same-row bool pairs; rewrite docs `:45-54`.
  - verify: a test fails on any curated tooltip equal to its descriptor name or containing "Nuke"; the docs group list matches the tables.
  - size: L
- [x] M253.P4.T7 — Check the layout in a remote GUI run
  - files: `Tests/gui/m253_render_settings_layout.py`, reusing `Tests/gui/m253_render_settings.py` and `Tests/gui/guitest.py:156`
  - approach: At 1920x1080, capture each of the Controls, Storm, Cycles and MoonRay tabs in default state and with every group expanded, one file per screenshot; if an expanded page exceeds the window, take successive scroll positions as separate files. Assert group order and open state, indents, and grey states after toggling Use Denoise, Use Guiding and Use Light Tree.
  - verify: `tools/ci/remote/build.sh release --from-run <id> --gui "Tests/gui/m253_render_settings_layout.py"` passes; the Cycles and MoonRay default states show no scrollbar; expanding and collapsing nested groups keeps row order; the user approves the screenshots.
  - size: M

- [x] M253.P4.T8 — Custom pixel jitter with no sample can't crash
  - files: charlesangus/cycles natron/deep-5.2 `src/scene/integrator.cpp`, `tools/ci/local/Dockerfile` (pin)
  - approach: `Integrator::device_update` reads elements 0 and 1 of the custom pixel jitter array when both bools are on. Treat an array with fewer than two values as off.
  - verify: a Cycles render with both bools on and an empty array doesn't crash, covered by the nudge test turning them both on.
  - size: M

**Verification gate:** CI green; every setting either delegate declares is reachable through a curated or a generated knob (audit output attached to the PR); settings persist and drive renders; a Xvfb screenshot of both property panels is shared with the user.

## Phase 253.2: Render settings in the scene (user, 2026-11-03)
- [x] M253.P2.T1 — Confirm whether hdCycles and hdMoonray consume HdRenderSettings prims (1ae36579d)
  - files: `PLAN/MILESTONES/M253-delegate-render-settings.md` (Decisions only)
  - approach: the sources aren't in build/, natron-dev or the local ghcr images (both build scripts delete them; `tools/ci/local/build-cycles.sh:169`, `build-moonray.sh:267`). Upstream, Cycles v5.2.0 `src/hydra/render_delegate.{h,cpp}` has no `GetRenderSettingsNamespaces` override and its Bprims (cpp:168) lack `renderSettings`. hdMoonray @986121d (openmoonray d96c6e30) `lib/hydramoonray/RenderDelegate.cc:187` lists only `renderBuffer`. Both take settings through `SetRenderSetting` only. Record this with links. If the image's pins have changed, re-read them. If either now supports the prim, T3 and T6 call `UsdImagingGLEngine::SetActiveRenderSettingsPrimPath` (engine.h:544) for it instead.
  - verify: Decisions note cites file:line for both delegates and the pinned tags
  - size: S
- [x] M253.P2.T2 — Carry `renderSettingsPrimPath` stage metadata through scene payloads (1ae36579d)
  - files: `Engine/ScenePayload.h`, `Engine/Nodes/Scene/SceneOps.cpp`, `USDRead.cpp`, `USDWrite.cpp`, `SceneNodeAuthoring.cpp`
  - approach: add the field to `SceneMetadata` (ScenePayload.h:45), comparing it in `operator==`. Then handle it the same way `defaultPrim` is handled: read it in SceneOps.cpp:331, write it onto the root in `writeRootMetadata` (:354), carry it in USDRead.cpp:321, write it in USDWrite.cpp:587, and hash it in `appendUpstreamMetadata` (SceneNodeAuthoring.cpp:216). This lets `UsdRenderSettings::GetStageRenderSettings` see it.
  - verify: a test where a .usda with the metadata passes through USDRead→USDWrite, and the composed stage and the written file both carry it
  - size: M
- [x] M253.P2.T3 — Prim-to-delegate translation library (1ae36579d)
  - files: new `Engine/Nodes/Scene/RenderSettingsPrim.{h,cpp}`, `Tests/Scene/RenderSettingsPrim_Test.cpp`
  - approach:
    - Namespaces come from the delegate's `GetRenderSettingsNamespaces()`. If that is empty, use a table (cycles, moonray, storm, otherwise `scriptPrefix()`).
    - Attribute names: a key that already starts with `<ns>:` is used as is (all of Cycles' keys and MoonRay's `sceneVariable:` keys); any other key becomes `<ns>:<key>` (MoonRay's `debug`, `logLevel` and so on, Storm's keys).
    - `settings(prim, renderer, time)` = `UsdRenderComputeNamespacedSettings` (usdRender/spec.h:121), mapped back to the delegate's keys.
    - `activePrim(stage, overridePath)` tries the override, then the stage metadata, then nothing.
    - It also reads the standard attributes: camera rel, resolution, pixelAspectRatio, aspectRatioConformPolicy, dataWindowNDC, disableMotionBlur, disableDepthOfField.
  - verify: unit tests on in-memory .usda strings: mapping both ways for prefixed and unprefixed keys, other namespaces ignored, and active-prim precedence
  - size: M
- [x] M253.P2.T4 — USDRenderSettings scene node with standard attributes (1ae36579d)
  - files: new `Engine/Nodes/Scene/USDRenderSettings.{h,cpp,reg.inc}`, `Tests/Scene/USDRenderSettings_Test.cpp`
  - approach: follow `USDCamera` (SceneNodeAuthoring, SceneLayerMemo).
    - It authors a `UsdRenderSettings` prim at a Prim Path knob, default `/Render/rendersettings`.
    - Knobs: Camera (a menu of the upstream cameras, authored as the relationship); format (`ExtentKnobs::createFormatKnobs`) authoring resolution and pixelAspectRatio; Conform Policy covering all five tokens; Data Window; Disable Motion Blur; Disable Depth Of Field.
    - It also has a "Make Active" bool, default on, which sets `renderSettingsPrimPath` (T2).
    - The UsdRenderSettingsBase attributes are uniform, so values are authored as defaults and the standard knobs don't animate (PM, 2026-11-03).
  - verify: the test reads the authored prim back through the UsdRender API, including that no time samples are authored
  - size: L
- [x] M253.P2.T5 — Move the per-renderer knob builders onto USDRenderSettings and drop the hidden store (1ae36579d)
  - files: `RenderSettingsKnobs.{h,cpp}`, `CyclesSettings.cpp`, `MoonRaySettings.cpp`, `USDRenderSettings.cpp`, `Tests/Scene/RenderSettingsKnobs_Test.cpp`
  - approach: give each `rendererIds()` entry its own page (tab) holding its curated group and Other Settings, all always shown. Add `author(prim, edit)`, which writes non-default values as namespaced attributes (T3's naming). Delete `kRenderSettingsParamStore`, `finish()`, `knobsLoaded()`, `orphans()` and `showRenderer()`. Do not import old values (no backward compat).
  - verify: values for every renderer round-trip through project save/load; the Cycles and MoonRay params→knob audits pass on the new node; the authored attribute names match T3
  - size: L
- [x] M253.P2.T6 — Default-renderer attribute `natron:renderer` (1ae36579d)
  - files: `USDRenderSettings.cpp`, `RenderSettingsPrim.{h,cpp}`, `Tests/Scene/RenderSettingsPrim_Test.cpp`
  - approach: a Default Renderer knob authors `uniform token natron:renderer` with the Hydra plugin id (e.g. `HdCyclesPlugin`).
    - USD has no schema attribute for picking a renderer. `UsdRenderPass.passType` is a pipeline category and would need a RenderPass prim.
    - Putting it on the prim lets several settings prims target different renderers, with the stage metadata choosing the scene default.
    - The `natron:` namespace is never one of a delegate's namespaces, so it is never forwarded.
    - The plugin id is the key `HdRendererPluginRegistry` and the Renderer knobs already use.
  - verify: `rendererFor(prim)` returns the authored id; an unknown id is a named error, not a silent fallback to Storm
  - size: M
- [x] M253.P2.T7 — Make USDRender a thin consumer: Settings Prim choice and Renderer override (4fe9eb7af, gate pending)
  - files: `Engine/Nodes/Scene/USDRender.{h,cpp}`, `HydraRenderImpl.{h,cpp}`, `Tests/Scene/USDRender_Test.cpp`, `USDRenderCycles_Test.cpp`, `USDRenderMoonRay_Test.cpp`
  - approach:
    - Remove `_settings` (USDRender.cpp:197-234, 538) and the camera, format and fit knobs.
    - Add a Settings Prim menu: "Stage Default" plus every `/Render` RenderSettings path.
    - Turn Renderer into "Scene Default" plus the delegates.
    - Precedence: the override knob, then `natron:renderer`, then Storm. Report it when no prim is found.
    - Build the request from T3: settings go through `SetRendererSetting` (HydraRenderImpl.cpp:848), and the camera, resolution and conform policy come from the prim. Map conform policy to `FitModeEnum` and add `expandAperture`. `adjustPixelAspect` warns.
  - verify: a precedence test (override > prim > Storm; Settings Prim override > metadata); a test that loads a hand-written .usda's /Render prim through USDRead and sees its cycles:samples, resolution and camera honoured
  - size: L
- [x] M253.P2.T8 — Hash only the active delegate's namespaces (4fe9eb7af, gate pending)
  - files: `Engine/Knob.h`, `Engine/Node.{h,cpp}`, `RenderSettingsKnobs.cpp`, `USDRender.cpp`, `Tests/Scene/USDRenderSettings_Test.cpp`
  - approach: renderer-tab knobs get a hash namespace. `getKnobsContentDigest` (Node.cpp:~900) skips them and folds them instead into a per-(prim, namespace) digest side table. That table also carries the node's default renderer and Make Active. `computeContentHash` (:950) merges it down the inputs. The node's `SceneLayerMemo` key still includes these knobs. USDRender's `appendNodeContentHash` folds only the active entry's digests; when the active prim isn't in the table (it comes from a file), it folds every namespace.
  - verify: editing a MoonRay knob while Cycles is active leaves USDRender's hash unchanged; switching to MoonRay afterwards changes it and renders the edited value (no stale cache hit)
  - size: L
- [x] M253.P2.T9 — Viewer reads the active prim (4fe9eb7af, gate pending)
  - files: `Gui/SceneViewport.{h,cpp}`, `Tests/SceneViewport_Test.cpp`
  - approach: the viewer applies T3's settings for its renderer from the stage's active prim. M254 - Progressive Viewer Render's Max Samples and Stop stay viewer-local and are applied last, over `cycles:samples` / `moonray:sceneVariable:pixel_samples`. Add a "Scene Default" entry to the dropdown, keeping Storm as the default per the M248 user decision of 2026-10-27. Restart only when the changed paths touch the active namespaces or anything outside /Render. MoonRay in the progressive viewer too: the viewer engine calls `MoonRayAovs::configureEngine` (its AOVs and thread budget), as USDRender's does; M254 was built before MoonRay landed.
  - verify: strict GUI runner: a Cycles viewer takes the prim's samples, Max Samples overrides it, and a MoonRay-tab edit does not restart refinement
  - size: M
- [x] M253.P2.T10 — GUI check (4fe9eb7af, gate pending)
  - files: `Tests/gui/m253_render_settings.py`
  - approach: a remote GUI check (`build.sh --gui`) at 1920x1080: a USDRenderSettings panel with Cycles, MoonRay and Storm tabs, USDRender's thin panel, and the viewer dropdown
  - verify: screenshots shared one per file; the user OKs them
  - size: M
- [x] M253.P2.T11 — Docs (4fe9eb7af, gate pending)
  - files: new `Engine/Nodes/Scene/docs/USDRenderSettings.md`, `docs/USDRender.md`
  - approach: describe the node, tabs, `natron:renderer`, precedence, the hash rule, and loading external /Render prims
  - verify: the docs build; the knob labels match the script names
  - size: S

**Verification gate:** CI green; T7/T8 tests pass; audits attached; GUI screenshots OKed.

## Decisions

- 2026-11-07 — F3 - Cycles And MoonRay Delegates UAT questions closed (user): patching MoonRay's denoiser to OIDN and building Cycles with `openpgl` are follow-ups, not part of F3, and no milestone exists for them yet. Current behaviour is fine for the render settings' open and closed groups, M251's Lobby camera exposure, M254's display transform on progressive frames, and the session on thread budget changes.

- 2026-11-06 — Panel polish before the user's look: MoonRay's Disable Lighting crashed on a first render because `setDisableLighting` used a null `mRenderIndex` (`RenderDelegate.cc:717`); patched in `build-moonray.sh` so the control stays. Numbers of magnitude 1e6 or more now show compactly in every `SpinBox`, since the 50 px field can't hold them; values aren't capped. Bounces and Clamping start closed so the default Cycles tab has no scrollbar: the user was asked whether to close Denoise instead.
- 2026-11-06 — Stack heads for M249 - Bundle Cycles Delegate, M250 - Bundle MoonRay Delegate, M251 - Cycles Rendering, M252 - MoonRay Rendering and M254 - Progressive Viewer Render merged here, with Cycles pin f2a142129. An earlier push (a207ffff7) carried conflict markers by PM error and was reverted by force-push within a minute; no build ran on it. A guard for the progressive worker's Hydra exceptions was added here, since this branch holds both sides.
- 2026-11-06 — P4.T6 source findings, all stated in the tooltips: Cycles' Path Guiding isn't compiled into our build (no openpgl in `hdCycles.so`), so its group does nothing. MoonRay's denoise likely does nothing: `ArrasSettings::setDenoiseEngine` is never called, so the engine stays OPTIX, which `libdenoiser.so` doesn't support here, and nothing sets `denoiser_input` for the guides (traced in upstream, not run). Use Pixel Jitter plus Use Custom Pixel Jitter Sample with an empty array can crash Natron (`integrator.cpp:391-393`, traced, not run); M253.P4.T8 guards it.
- 2026-11-06 — P4.T3: Direct Light Sampling Type sits under `WITH_CYCLES_DEBUG` in Cycles (`integrator.cpp:66-75`) and isn't in our build, so it has no knob. Volume Max Steps and Step Rate grey with Volume Ray Marching off (`shade_volume.h:2127`, `volume.cpp:1222-1242`). `Session::update_scene()` (`session.cpp:724-730`) overwrites Use Sample Subset, Sample Subset Offset and Sample Subset Length on every update, and AA Samples on a reset; they stay as controls and T6's tooltips say so. Use Pixel Jitter plus Use Custom Pixel Jitter Sample with an empty array reads out of bounds in Cycles (`integrator.cpp:391-393`); not guarded here, T6 should check whether the delegate can reach it. The nudge loop now moves about 45 promoted keys together and has never rendered that way, so watch it at the build.
- 2026-11-05 — Phase 253.4 added at the user's request; layout from a consultant's survey of Blender's Cycles panels, Karma, MoonRay's SceneVariables and Gaffer. PM's choices, open to the user at the screenshots: Sampling, Denoise and Light Paths (Bounces, Clamping) open by default, everything else closed; if T7 shows a default-state scrollbar on Cycles, close Clamping first. New MoonRay controls (Fireflies Removal, `sample_clamping_value`) are a follow-up, not this phase.
- 2026-10-28: Implemented (build pending; waits on M250's image). Each renderer gets a curated group plus a closed "Other Settings" group with a knob for every other declared setting; only the chosen renderer's groups show. Values for missing renderers/settings kept in a hidden hash-exempt store, logged once, restored when the renderer returns. Labels drop the renderer prefix, script names keep it. Only non-default values are sent. A settings change rebuilds the engine (Cycles doesn't mark samples/time limit dirty). Storm's settings are read from a Storm delegate made without Hgi. Gaps: Cycles declares no pixel filter; MoonRay has no seed; MoonRay curated sampling uses undeclared scene-variable keys. M254's Max Samples spin box should read/write cycles:samples / moonray:sceneVariable:pixel_samples. Viewer's own copy of the settings not implemented. Open (user): OIDN in our Cycles and MoonRay builds (M249 OIDN +46 MB question)? Should unselected renderers' knobs change the node hash (they do now)?
- 2026-10-27 — **Curated plus generic fallback (user):** hand-built groups for the two shipped delegates, generated knobs for everything else.
- 2026-11-02 — Audit: all four tasks at c30a7cd04 (RenderSettingsKnobs, CyclesSettings, MoonRaySettings, audit output in the Cycles and MoonRay tests). Pushed, clean, no PR, 23 behind origin/main; stacked on M251 and M252, so it waits for them. The open user questions in the 2026-10-28 note are unanswered (audit).
- 2026-11-02 — OIDN is bundled (user).
- 2026-11-03 — User on the hash question: hidden renderers' settings must not change the hash, and probably should not be editable at all ("why would we show knobs that aren't relevant?"). The user also questions whether one USDRender node is the right shape and asked how Solaris, Katana and Gaffer handle several renderers; a consultant is researching before this is re-asked. Blocked on that design question.
- 2026-11-03 — **Reshape before merge (user, after research on Solaris/Katana/Gaffer/USD):** a USDRenderSettings scene node authors /Render RenderSettings prims (camera, resolution and other standard attributes) with **one tab per installed renderer** (curated group + Other Settings, namespaced attributes; all renderers' values coexist); **the default renderer is authored in the scene** and USDRender and the viewer may override it; USDRender becomes a thin renderer-agnostic consumer choosing a delegate and a settings prim (default from stage metadata); the hidden-value store goes; the hash ignores /Render attributes outside the active delegate's namespaces. M254's Max Samples/Stop stay viewer-local overrides; M256 takes RenderProduct/RenderVar for AOVs. Needs a check whether hdCycles/hdMoonRay read HdRenderSettings prims natively (else the host translates the prim into SetRendererSetting). New phase to be elaborated.
- 2026-11-03 — Phase 253.2 elaborated (consultant): neither hdCycles (v5.2.0) nor hdMoonray reads HdRenderSettings prims, so the host translates the active prim's namespaced attributes into SetRendererSetting. Phase 253.1's settings-on-USDRender and hidden store are superseded by it. M253 merges after M254. Open (user): should the viewer default stay Storm or become Scene Default?
- 2026-11-03 — User: Cycles can't really be tested without render settings; proceed with Phase 253.2 and UAT M249/M251/M252/M254 together with M253 (one AppImage). M252 and M254 merged in (merges 2026-11-03).
- 2026-11-03 — T1: neither delegate consumes HdRenderSettings prims; both take settings only through SetRenderSetting, so the host translates the prim. Cycles charlesangus/cycles@c8d449e6 (v5.2.0+deep): src/hydra/render_delegate.cpp:58-64,168 (Bprims renderBuffer/openvdbAsset only), no GetRenderSettingsNamespaces; settings via SetRenderSetting (cpp:133, 401). MoonRay openmoonray v2026.29.1/d96c6e30, hdMoonray@986121db: lib/hydramoonray/RenderDelegate.cc:187-193 (Bprims renderBuffer/openvdbAsset), no namespaces or SetRenderSetting override (base store; RenderSettings.cc:70,86,109).
- 2026-11-03 — T3: UsdRenderComputeNamespacedSettings reads defaults only, so settings() reads each attribute at the requested time. T5: author(prim) takes the prim, not the layer edit.
- 2026-11-03 — T7–T11 at 4fe9eb7af. USDRender reads only camera, resolution, pixel aspect and conform policy from the prim; Data Window and the motion-blur/DoF switches are authored but not yet consumed (documented). Follow-up in progress: consumers other than USDRender fold every namespace digest so USDWrite etc. see renderer-tab edits.
- 2026-11-04 — PR #188 review fixes. Open follow-up to check: SceneReparentNode moves prims, so a hash-namespace table's prim path could go stale below it; USDCamera is assumed to carry no untracked render settings.
