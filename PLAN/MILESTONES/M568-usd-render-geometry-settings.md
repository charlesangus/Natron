# M568 - USD Render Geometry Settings

A scene node that writes per-prim renderer settings onto the prims a pattern matches: holdout (matte), shadow catcher, per-ray-type visibility and the other object-level switches Cycles and MoonRay have, so one object of a scene can be treated differently from the rest without a global render setting. It follows the Solaris Render Geometry Settings LOP, and covers the Shadow Catcher and Background Plate use cases as knobs on it. Settings are authored as constant primvars, which USD inherits down the hierarchy, so a setting on a parent reaches its children; each delegate reads the namespace it knows and Storm reads none. The knob machinery is `AuthoredKnobSet` (`Engine/Nodes/Scene/AuthoredKnobSet.h`): a value knob writes an opinion only once the user touched it, which is the reference node's "Do Nothing / Set or Create" control menu.

What the delegates read today, found in their sources: hdMoonray turns `primvars:moonray:<rdl attribute>` into an override of the same-named RDL attribute (`build/forks/hdMoonray/lib/hydramoonray/Primvars.cc:183`) and reads nine named visibility primvars (`moonray:visible_in_camera`, `visible_shadow`, `visible_diffuse_reflection`, `visible_diffuse_transmission`, `visible_glossy_reflection`, `visible_glossy_transmission`, `visible_mirror_reflection`, `visible_mirror_transmission`, `visible_volume`, in `GeometryMixin.cc` ~line 278), `moonray:side_type`, and `moonray:smooth_normal`. The Cycles delegate (`.local/forks/cycles/src/hydra/`) reads no per-object primvar at all: `geometry.inl` sets only `set_visibility(IsVisible() ? ~0 : 0)`, although Cycles' `Object` has `visibility`, `use_holdout`, `is_shadow_catcher`, `is_caustics_caster`, `is_caustics_receiver`, `shadow_terminator_shading_offset` and `shadow_terminator_geometry_offset` sockets (`src/scene/object.h`). So Cycles needs a fork change before any setting shows; there are no existing Cycles primvar names to match. This milestone defines them as `cycles:object:<socket>` in the render-settings convention `cycles:<node>:<socket>` that `render_delegate.cpp` already uses, with the visibility sockets as `cycles:object:visibility:<ray type>` booleans. MoonRay's holdout or matte attribute is not in the RDL sources searched (`Geometry.h` has visibility, ray epsilons, shadow receiver label and exclusion mappings, and local motion blur); Phase 1 settles it.

## Scope

**Solaris Render Geometry Settings controls (Karma tab), each In or Out:**
- Frame Range/Subframes group (Sampling Behavior, Start/End/Inc, Subframe Sampling, Shutter, Shutter Open/Close, Camera Prim, Samples, Always Include Frame Sample): Out. Scene nodes author at the current time and the knobs animate; these only matter for baking time samples, which Natron does not do in nodes.
- Primitives: In (`Prim Pattern`, a prim pattern knob).
- Initialize Parameters: Out. The Authored toggles replace the control menu; reading the current value from the stage is not offered.
- Per-parameter control menu (Set or Create, Set if No Authored Value, Set if Exists, Append/Prepend/Remove Prims, Add, Multiply, Block, Disconnect Input, Do Nothing): Do Nothing and Set or Create are In (the Authored toggle). The rest are Out: this node writes constant primvars only, never relationships or numeric arithmetic on existing values, and a Block is "turn the toggle off".
- Motion Blur: Enable Motion Blur is In (`Motion Blur`; Cycles `cycles:object:use_motion_blur` turns the object's blur off, MoonRay `moonray:use_local_motion_blur` if Phase 1 shows it does that). Motion Samples from Stage, Velocity Blur, Geometry Time Samples, Transform Time Samples, Motion Blur Style, Instance Velocity Blur, Instance Motion Samples, Volume Velocity Blur Mode, Volume Velocity Blur Scale, Volume Velocity Lattice Quality: Out. Neither delegate has a per-prim equivalent: Cycles takes its samples from Hydra's sampled transforms (`src/hydra/util.cpp:82`) and MoonRay from the scene's motion steps.
- Sampling: Diffuse Quality, Reflection Quality, Refraction Quality, Volume Quality, SSS Quality, Volume Step Rate, Volume Shadow Step Rate, Volume Sampling Field, Volume Filter, Volume Filter Width, Secondary Noise Level, Min Secondary Samples, Max Secondary Samples: Out. Karma sampling model; Cycles and MoonRay have no per-object sample counts. Volume Step Rate has a Cycles global (`volume_step_rate`) already in the Cycles tab of USDRenderSettings.
- Limits: Diffuse Limit, Reflection Limit, Refraction Limit, Volume Limit, SSS Limit, Limit Components, Limit Color: Out. Per-object bounce limits are a Karma feature; the global bounces are in USDRenderSettings.
- Uniform Volume, Uniform Volume Samples: Out. Karma-only.
- Shading: Render Visibility is In, as the Visibility group below (Camera, Shadow, Diffuse, Glossy, Transmission, Volume Scatter, each a toggle). Holdout Mode is In as `Holdout` (an off/on toggle plus the Shadow Catcher toggle; Karma's Background and Phantom modes are Out, being its holdout-AOV workflow). Stencil Map, Stencil Threshold, Cusp Angle, Custom Tracesets, LPE Tag, Enable Internal Reflection, Thinwall at Refract Limit, SSS Traceset, Direct Refraction Subset, Dielectric Priority: Out (Karma-only or no delegate equivalent; trace sets are collections, see M566 - USDCollection Node). Fix Shadow Terminator and Bump Shadow Terminator are In as `Shadow Terminator Shading Offset` and `Shadow Terminator Geometry Offset` for Cycles only (the two Cycles sockets).
- Caustics (Enable Caustics, Roughness Clamp) and Fake Caustics (Evaluate BSDF, Color, Opacity, Fresnel Darken): Enable Caustics is In as `Caustics Caster` and `Caustics Receiver` for Cycles. The rest are Out (Karma-only).
- Light (Efficient Emission Sampling, Is Portal, Portal Dome Lights): Out. Light settings belong on `USDLight`.
- Geometry (Render Points As, Render Curves As, Override Curves Basis, Width Scale, Cull Backface): Out, except that MoonRay's existing `moonray:side_type` is In as `Double Sided` (Single, Double; MoonRay only, it is the one geometry switch with a read path).
- Dicing: Dicing Quality is In, as the per-prim subdivision knobs (user, 2026-11-24, kept here rather than in M494 - Delegate Subdivision): Cycles `Dicing Rate` (`cycles:dicing_rate`, a multiplier on the global rate) and `Max Subdivision Level` (`cycles:max_level`); MoonRay `Mesh Resolution` (`moonray:mesh_resolution`) and `Adaptive Error` (`moonray:adaptive_error`). The delegates read these names in M494's fork work; this node only authors them. Displacement Style, Dicing Minimum Depth, Dicing Maximum Depth, Dicing Predisplace, Dicing Z-Importance: Out (displacement is not supported by either delegate).
- Texture Baking (Low Resolution Object ID, Cage Object ID, High Resolution Object ID, Identifier Traceset, Visibility Traceset, Visibility Cull Backface): Out. No baking workflow.
- Outline (Enable, Radius, Contour, Id Threshold, Crease, Crease Threshold, Depth, Depth Threshold, Secondary): Out. Karma-only.

**Shadow Catcher LOP controls:**
- Primitives: In (the same `Prim Pattern`); Blocks Lighting: Out (no delegate equivalent); Replace Prim Material and Material Path: Out. Binding a grey material is `USDAssignMaterial`'s job, so the node stays attribute-only.
- The shadow-catcher behaviour itself is the `Shadow Catcher` toggle: object invisible to the camera, receives shadows. Cycles `is_shadow_catcher`; MoonRay: per Phase 1.

**Background Plate LOP controls:**
- Hold-Out Primitives: In (the `Holdout` toggle). RenderSettings Prim, Material Path Prefix, Allow Shader Parameter Animation, Plate, Scale, Translate, Use Offscreen Texture, Offscreen Texture, Offscreen Color, Rotate, the holdout AOV setup and the shader parameters (Metallic, Reflectivity, ...): Out. The plate is composited in the compositing graph in Natron, and the render-settings background image and holdout RenderVars have no delegate support here.

**In:**
- `USDRenderGeometrySettings` with groups Visibility (six ray-type toggles), Holdout and Shadow Catcher, Motion Blur, Subdivision (the four dicing primvars above), Cycles (the offsets and caustics) and MoonRay (Double Sided), each value knob behind an Authored toggle.
- A fork change in charlesangus/cycles (`natron/v2026.29.1` line, branch recorded by the PM) so the Hydra delegate reads `cycles:object:*` primvars on each object, and a bump of the image pin in `tools/ci/local/Dockerfile`.
- Per-delegate mapping of each knob, authored as one primvar per delegate namespace the knob has a mapping in; a delegate with none gets nothing.
- Docs `Engine/Nodes/Scene/docs/USDRenderGeometrySettings.md`, toolbar entry in the USD Render submenu (M520), icon, tests.

**Out:**
- The global Subdivision Surfaces toggle, global dicing settings and the delegates' reading of the dicing primvars: M494 - Delegate Subdivision. This node authors the per-prim primvars only; they take effect once M494's delegate work has landed, and the docs say so until then.
- Displacement settings: none.
- Storm support for any of it: Storm ignores the primvars (user).
- Light settings, camera settings: none.
- A MoonRay fork change for holdout or shadow catcher if Phase 1 finds no attribute to map: the knob then has a tooltip saying MoonRay does not support it, and the finding is a follow-up.
- Karma's holdout AOV set (`Holdout Diffuse`, `Holdout Shadows`, ...): none.

**Edge cases:**
- Setting authored on a parent: children inherit it as a constant primvar; a child that authors its own wins.
- A matched prim that is not a gprim or an Xform: the primvar is authored (it inherits), no warning.
- A knob with no mapping for the active renderer: authored for the delegates that have one; the tooltip names which delegates use it.
- Shadow Catcher on: also hides the object from the camera in Cycles (its socket does); for MoonRay the mapping is whatever Phase 1 finds, or unsupported.
- Visibility and Holdout both set: both are authored as given; the delegate decides.
- Prims in unloaded payloads are not matched and no payload is loaded.
- A viewer render under Storm shows no change; the node docs and tooltips say so.

**Assumptions:**
- The Cycles fork is ours to patch (M494 changes it too); this milestone sequences after M494 so the two do not fight over the fork and the pin.
- Cycles accepts these object sockets set from Hydra at sync time without a rebuild of the scene; an object attribute change re-syncs the prim, so a knob edit restarts the progressive render.

## Phase 568.1: Mapping audit

- [ ] M568.P1.T1 — Verify every delegate attribute against the sources
  - files: `build/forks/hdMoonray/lib/hydramoonray/*.cc`, `.local/forks/scene_rdl2-src/lib/scene/rdl2/Geometry.h`, `.local/forks/openmoonray`, `.local/forks/cycles/src/{hydra,scene}/` (read only), this milestone file
  - approach: for each In knob in Scope, find the exact RDL attribute (MoonRay) or `Object`/`Geometry` socket (Cycles), whether `Primvars.cc`'s generic `moonray:` override reaches it, and the value type each takes (bool or int for the visibility primvars). Specifically settle MoonRay holdout and shadow catcher: search `Geometry`, `Layer`, material and light RDL classes for holdout, matte and shadow-catcher attributes. Write the table (knob, Cycles primvar, MoonRay primvar, notes) into `## Decisions`.
  - verify: the table has a row per In knob with source file and line for each mapping; unmapped cells say "none".
  - size: M

## Phase 568.2: Cycles fork

- [ ] M568.P2.T1 — Read object primvars in the Cycles delegate
  - files: `.local/forks/cycles/src/hydra/geometry.inl` (and `curves.cpp`/`mesh.cpp` as needed), `.local/forks/cycles/src/hydra/util.{h,cpp}`, `tools/ci/local/Dockerfile`
  - approach: on sync of every geometry, read the constant primvars `cycles:object:visibility:{camera,diffuse,glossy,transmission,shadow,scatter}` (bool), `cycles:object:use_holdout`, `cycles:object:is_shadow_catcher`, `cycles:object:is_caustics_caster`, `cycles:object:is_caustics_receiver`, `cycles:object:shadow_terminator_shading_offset`, `cycles:object:shadow_terminator_geometry_offset`, `cycles:object:use_motion_blur` (forces off only) through the existing primvar readers, and set them on every `Object` instance together with the prim's own visibility. Absent primvar leaves the Cycles default. Re-sync when a primvar changes. Commit to the fork branch, then bump `NATRON_CYCLES_COMMIT` and its SHA256 in `tools/ci/local/Dockerfile`. Rebase on M494's fork changes first.
  - verify: the fork builds in the image (release gate); `USDRenderGeometrySettings_Test` Cycles cases (Phase 3) pass; with no primvars authored the existing Cycles goldens do not change.
  - size: L

## Phase 568.3: The node

- [ ] M568.P3.T1 — USDRenderGeometrySettings class and knobs
  - files: `Engine/Nodes/Scene/USDRenderGeometrySettings.{h,cpp}`, `Engine/Nodes/Scene/USDRenderGeometrySettings.reg.inc`
  - approach: a scene node on `AuthoredKnobSet`, like `USDSetAttribute`'s primvar path (`SceneNodeAuthoring::authorValue()`; constant interpolation through `UsdGeomPrimvarsAPI`). Knobs (labels equal the readable script names): Prim Pattern (`primPattern`); Subdivision group Dicing Rate (`dicingRate`), Max Subdivision Level (`maxSubdivisionLevel`), Mesh Resolution (`meshResolution`), Adaptive Error (`adaptiveError`); Visibility group Camera, Shadow, Diffuse, Glossy, Transmission, Volume Scatter (`visibleToCamera` ... ); Holdout (`holdout`); Shadow Catcher (`shadowCatcher`); Motion Blur (`motionBlur`); Cycles group Shadow Terminator Shading Offset, Shadow Terminator Geometry Offset, Caustics Caster, Caustics Receiver; MoonRay group Double Sided. A table maps each knob to its primvar names per delegate (from M568.P1.T1) and the node writes each mapped name. Tooltips say what each does and which delegates use it, and that Storm ignores them.
  - verify: gtest on an in-memory stage (the four dicing primvars included, as constant float or int primvars as M494 reads them) — each knob's authored toggle on writes exactly the mapped constant primvars with the right types; toggle off writes nothing; a parent setting is inherited by a child under `UsdGeomPrimvarsAPI::FindPrimvarWithInheritance`.
  - size: L

- [ ] M568.P3.T2 — Docs, toolbar entry, icon
  - files: `Engine/Nodes/Scene/docs/USDRenderGeometrySettings.md`, `Gui/Resources/Images/NativeNodes/USDRenderGeometrySettings.png`
  - approach: docs in the style of the other scene nodes: every knob with the delegates that use it (the Phase 1 table), inheritance, that Storm shows nothing; no reference-app names. Grouping `PLUGIN_GROUP_USD_Render` (M520), or `PLUGIN_GROUP_3D_USD` if M520 has not landed.
  - verify: the node shows in the submenu; the docs name every knob.
  - size: M

- [ ] M568.P3.T3 — Params-to-knob audit
  - files: this milestone file (a checked table)
  - approach: walk the Render Geometry Settings, Shadow Catcher and Background Plate control lists in Scope against the built knobs; each In control has a knob and a doc line.
  - verify: the table in the PR body maps every control.
  - size: S

## Phase 568.4: Render checks

- [ ] M568.P4.T1 — Per-delegate render tests
  - files: `Tests/Scene/USDRenderGeometrySettings_Test.cpp`, `Tests/CMakeLists.txt`
  - approach: parametrised over delegates with `INSTANTIATE_TEST_CASE_P`, a sphere over a plane lit by a light: Visibility camera off hides the sphere in the beauty but keeps its shadow; Holdout makes it transparent in alpha; Shadow Catcher on the plane leaves only the sphere's shadow; Shadow visibility off removes the shadow. Delegates the build lacks return early (as the existing delegate tests do, no `GTEST_SKIP`); cases a delegate has no mapping for expect no change.
  - verify: release gate runs them green; with the node absent the Cycles and MoonRay goldens are unchanged.
  - size: L

- [ ] M568.P4.T2 — Xvfb GUI check
  - files: `Tests/gui/usd_rendergeometrysettings.py`
  - approach: build sphere + plane + light → USDRenderGeometrySettings in a delegate viewer, toggle Camera visibility on the Authored toggle and assert the progressive render restarts and the sphere disappears (a knob-driven refresh); screenshots to the user.
  - verify: the script passes under Xvfb at 1920x1080; the user OKs the screenshots.
  - size: M

**Verification gate:** release gate green per `.local/build-mode` with the bumped Cycles pin; `USDRenderGeometrySettings_Test` passes on each delegate the build has; the unchanged Cycles and MoonRay goldens still match; `Tests/gui/usd_rendergeometrysettings.py` passes under Xvfb at 1920x1080; the user OKs the screenshots.

## Decisions

- 2026-11-24 — **Milestone now (user):** a node writing per-prim renderer primvars/attributes on a prim pattern: holdout/matte, shadow catcher, camera/shadow/reflection/etc visibility, subdivision, motion-blur samples, mapped per delegate (Cycles, MoonRay), Storm ignores. "Motion-blur samples" is narrowed to the on/off switch because neither delegate has a per-prim sample count (user accepted, 2026-11-24).
- 2026-11-24 — **Per-prim subdivision primvars stay in this milestone (user):** `cycles:dicing_rate`, `cycles:max_level`, `moonray:mesh_resolution`, `moonray:adaptive_error` are authored by this node, not handed to M494 - Delegate Subdivision (doing). M494 owns the global toggle and settings and the delegates' reading of the names; the dependency on M494 stays because both change the Cycles fork and its image pin, so this milestone rebases on M494's fork commits first.
- 2026-11-24 — **Cycles per-object primvar patch accepted (user):** the Cycles delegate fork reads `cycles:object:*` primvars (M568.P2.T1).
