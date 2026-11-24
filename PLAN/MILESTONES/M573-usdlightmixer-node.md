# M573 - USDLightMixer Node

A new scene node, USDLightMixer, whose settings panel is a table of every light on its input stage. Lights are the rows. The columns are Enable, Mute, Solo, Intensity, Exposure, Color and Color Temperature, plus optional Diffuse, Specular and Normalize. The node authors the edits as overrides in its own layer, the way `USDEditPrim::renderScene` (`Engine/Nodes/Scene/USDEditPrim.cpp`) does through `emitSceneLayer`. Each cell the user edits is backed by a real knob, paired through `AuthoredKnobSet` (`Engine/Nodes/Scene/AuthoredKnobSet.h`), so only touched values author, and they animate, take expressions and undo like any knob. A light gets its knobs the first time one of its cells is edited. Untouched rows show the input's values. The table is a custom `KnobGui` on a placeholder knob, the pattern `LoopDriverTable` (`Gui/LoopDriverTable.{h,cpp}`) uses and `KnobGuiFactory::createGuiForKnob` (`Gui/KnobGuiFactory.cpp`) dispatches, and M450 - USDWrite Export Preview plans for its tree. The table reads the input stage off the UI thread with the showing viewer's context (`SceneNodeAuthoring::getShowingViewerContext`, `SceneQueryPool::instance().borrow`), as M523 - USDSetAttribute Type From Stage does. Light groups follow the reference's collections: each group authors a `UsdCollectionAPI` and has master intensity, exposure and tint applied to its member lights. User decision: "Not a panel, a node" (2026-11-24).

## Scope

**In:**
- USDLightMixer: one scene input, registered in the M520 - USD Toolbar Category And Submenus Lights submenu, passing its input through unchanged until something is edited.
- Rows: every prim matched by Light Pattern (default: all lights) that has `UsdLuxLightAPI`, including mesh lights (`MeshLightAPI`) and inactive lights, so a disabled light keeps its row. Read-only Path and Type columns.
- Columns: Enable (authors `active`), Mute, Solo, Intensity, Exposure, Color, Color Temperature (`inputs:enableColorTemperature` plus `inputs:colorTemperature`). Diffuse, Specular and Normalize columns can be shown with toggles. These replace the reference's `sheet.show_attributes` config.
- Mute authors `visibility = invisible` on a muted light. Solo: while any light is soloed, every matched light that is not soloed authors `visibility = invisible`. Nothing authors for a light that is neither muted nor hidden by solo, so the input's visibility passes through. If P1.T5's probe finds a delegate that still lights the scene with an invisible light, Mute and Solo author `active = false` instead (user, 2026-11-24). The layer can't know which delegate will render it, so the fallback applies to every render.
- Per-light override knobs, created on first edit, grouped per light under a group labelled with the light's path, on an "Overrides" page, with a hidden path string knob. They serialize with the node and rebuild on load.
- Groups (the reference's collections): a name, Includes and Excludes prim patterns, and master Intensity (multiplied in), Exposure (added) and Tint (multiplied into Color). Each group authors `collection:<name>` on `/collections`, with expansion rule `explicitOnly`. A light in several groups takes the product of their masters. The table shows each group as a collapsible row with its members under it. A "New Group" button makes a group from the selected rows.
- Table niceties: a text filter on path; edited cells shown bold and untouched ones in the input's value; selecting rows selects those prims in the viewer and Scene Graph (the M232 path, `SceneViewport::setSelectedPrims`); cell edits go through the knob's undoable commands.
- Missing lights: overrides whose light no longer exists upstream keep their row, greyed out and marked missing, with a "Remove Override" context action. The node shows a persistent warning naming how many are missing.
- `docs/USDLightMixer.md`, ctest coverage, and an Xvfb GUI script with screenshots at 1920x1080.

**Out:**
- The reference's Sliders view (a light-board layout of the same values): the table edits the same values, and a second layout adds no capability (user, 2026-11-24). No milestone.
- The reference's light tree with drag-in of lights and collections, and adding the same light twice: every input light is listed, so there is nothing to drag in.
- Drag to rearrange rows: rows sort by path, groups by creation order.
- The reference's Transforms tab and its transform knobs (Transform mode, Transform Order, Translate, Rotate, Scale, Shear, Uniform Scale): USDEditTransform already moves lights.
- The `LightMixerConfig.json` file, including `attributes.config` (type, default, min, max, label, component_labels, modify, identity, precision): no config files. Ranges and precision come from the knob definitions, and modify/identity are fixed per master control.
- `main.default_tab`: one view only. `collections.search`: groups live in this node and author on `/collections`. `lights.search`: replaced by Light Pattern.
- Arbitrary extra attribute columns and per-component columns (`inputs:color$r`): only the listed UsdLux inputs.
- Light linking and shadow linking columns: no milestone yet (the survey's light-linking item).
- Light filters (`UsdLuxLightFilter`) and `ShadowAPI` inputs: not rows or columns. No milestone.
- Mute, Solo and Enable on group rows: not in the reference. Select the member rows and toggle them.
- A dockable, graph-wide light editor panel and Edit Scope routing: the user chose a node.

**Edge cases:**
- An animated upstream input: untouched cells show the value at the current time. An override replaces the attribute with the knob's value, or its time samples when the knob is animated (`SceneNodeAuthoring::authorValue` with `isAnimated`).
- A light under an instanceable prim (instance proxy) can't take an override. Its row is read-only, with a tooltip saying so.
- Lights inside unloaded payloads are not listed (no payload loading without user action). At render, Solo hides every non-soloed light the render's stage holds, so a light first seen at render is hidden while anything is soloed.
- Enable off on a light with children deactivates the children too. The tooltip says so.
- A renamed upstream light loses its overrides: they stay as a missing row until removed. There is no re-targeting.
- Group master values apply on top of the light's override or input value. A group whose patterns match nothing authors its collection with no members and warns.
- No viewer shows the node: the table shows "Not shown in a viewer" and lists nothing, as M523 skips its lookup. Override knobs and render still work.
- An upstream change while the listing runs: the stale result is dropped (generation counter), and the table keeps its last rows, marked updating, without blocking the UI.
- Many lights (hundreds): knobs exist only for edited lights, so the panel stays responsive. The table is a lazy model, not a widget per cell.

**Assumptions:**
- Storm, Cycles and MoonRay through Hydra drop lights authored `visibility = invisible`. Unverified; P1.T5 probes it, and a delegate that ignores it switches Mute and Solo to `active = false`.
- A project bundle (M16 - Project Format Redesign) serializes dynamically created knobs that a node rebuilds in `onKnobsAboutToBeLoaded`, as USDEditPrim's schema knobs do.

## Phase 573.1: Engine

- [ ] M573.P1.T1 — Add the USDLightMixer node
  - files: `Engine/Nodes/Scene/USDLightMixer.{h,cpp}` (new), `Engine/Nodes/Scene/USDLightMixer.reg.inc` (new), `Engine/CMakeLists.txt`, `Tests/Scene/USDLightMixer_Test.cpp` (new), `Tests/CMakeLists.txt`, `Gui/Resources/Images/NativeNodes/USDLightMixer.png`
  - approach: a scene node shaped like USDEditPrim, with one scene input, registered in the Lights submenu (M520's `PLUGIN_GROUP_USD_*`). Knobs: `lightPattern` (PrimSelector, default every light), `showDiffuse`, `showSpecular`, `showNormalize` (bools, default off), a placeholder string knob `mixerTable` (not serialized, the table's anchor), and an empty "Overrides" page. `renderScene` passes the input through when nothing is authored.
  - verify: gtest creates the node on a USDLight input and asserts the output stage equals the input, the knob names and labels (readable script names), and the toolbar grouping.
  - size: M

- [ ] M573.P1.T2 — List the lights of a stage
  - files: `Engine/Nodes/Scene/USDLightMixerLights.{h,cpp}` (new, USD-only like `USDSetAttributeLookup`), `Tests/Scene/USDLightMixerLights_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: `collectLights(stage, pattern, time) → std::vector<LightRow>`. Each row holds the path, the type name, whether it is an instance proxy, and its active, visibility and column values (intensity, exposure, color, enableColorTemperature, colorTemperature, diffuse, specular, normalize) read through `UsdLuxLightAPI` at time. Traverse with a predicate that includes inactive prims. Match through `PrimSelector` the way `SceneNodeAuthoring::matchPattern` does, keeping prims that `HasAPI<UsdLuxLightAPI>`.
  - verify: gtest on an in-memory stage: a sphere light, a rect light with animated intensity (value at two times), an inactive distant light (listed, active false), a mesh with `MeshLightAPI`, a light under an instanceable prim (instance proxy set), a light filter (not listed), and a pattern that narrows the result.
  - size: M

- [ ] M573.P1.T3 — Create, save and rebuild per-light override knobs
  - files: `Engine/Nodes/Scene/USDLightMixer.{h,cpp}`, `Tests/Scene/USDLightMixer_Test.cpp`
  - approach: `ensureLightOverrides(path)` creates, on the Overrides page, a group knob labelled with the path, a hidden `lightPath` string knob, and the value knobs `enable`, `mute`, `solo`, `intensity`, `exposure`, `color`, `enableColorTemperature`, `colorTemperature`, `diffuse`, `specular`, `normalize`. Each value knob is added through an `AuthoredKnobSet` and seeded from the light's input values without marking it authored. Script names are `light<N>_<attr>`, with N a stable index kept in the path knob's group name. `removeLightOverrides(path)` deletes a set. `onKnobsAboutToBeLoaded` recreates the sets listed in the serialization before the values load, as `USDEditPrim::onKnobsAboutToBeLoaded` does. Forward `knobChanged` to the sets.
  - verify: gtest creates overrides for two lights, edits intensity on one, serializes the node, loads it into a fresh node, and asserts both sets exist with the edited value authored and the other knobs not authored. Removing one set and reloading drops it.
  - size: L

- [ ] M573.P1.T4 — Author value overrides in the node's layer
  - files: `Engine/Nodes/Scene/USDLightMixer.cpp`, `Tests/Scene/USDLightMixer_Test.cpp`
  - approach: in `renderScene`, collect the authored knobs of each override set (`forEachAuthored`). In a `SceneLayerMemo::AuthorFunc` passed to `emitSceneLayer`, author `over` specs on each light path: the `inputs:*` values via `SceneNodeAuthoring::authorValue` (time samples when animated) and `active` for Enable. Skip instance proxies. The layer reads facts (`appendUpstreamMetadata` plus the override paths), as USDEditPrim does. Override sets whose path is missing upstream author nothing and set the persistent warning from Scope (cleared when none are missing).
  - verify: gtest: overriding intensity and color on one of two lights changes only those attributes in the output, the other light is untouched, an animated exposure knob writes time samples, Enable off writes `active = false`, and a missing path raises the warning and authors nothing.
  - size: M

- [ ] M573.P1.T5 — Author Mute and Solo as visibility
  - files: `Engine/Nodes/Scene/USDLightMixer.cpp`, `Tests/Scene/USDLightMixer_Test.cpp`, `Tests/Scene/USDRenderStorm_Test.cpp`, `Tests/Scene/USDRenderCycles_Test.cpp`
  - approach: first the probe. Add cases to the Storm, Cycles and MoonRay render tests that render a two-light scene with one light authored `visibility = invisible` and compare it with the one-light render within each test's existing tolerance. Then, in the same author function, gather the matched lights on the render's stage. A muted light, and while any light is soloed every non-soloed one, is hidden. Lights that are neither author nothing. A light that is both soloed and muted stays muted. Hiding goes through one function, `hideLight(prim)`, with one file-scope constant `kHideByActive` selecting `visibility = invisible` (`UsdGeomImageable`) or `active = false`. Set it from the probe: false when every delegate passes, true when any delegate ignores visibility on lights (user, 2026-11-24). When it is true, record the failing delegate in this file's Decisions, and make the Mute and Solo tooltips and `docs/USDLightMixer.md` say that hiding deactivates the light and its children. Under `active = false` the table keeps the row, since P1.T2 lists inactive prims, and Enable and Mute stay separate knobs. A light the user disabled stays disabled when unmuted.
  - verify: the probe cases pass, or fail only for the delegates recorded in Decisions with `kHideByActive` set. A gtest on three lights checks the attribute authored under the chosen constant for the mute-only, solo-one and solo-plus-mute cases, and that clearing both leaves the input's visibility and `active`. The render tests render a two-light scene with one light muted through USDLightMixer and match the one-light render on every delegate.
  - size: M

## Phase 573.2: Groups

- [ ] M573.P2.T1 — Author light groups as collections
  - files: `Engine/Nodes/Scene/USDLightMixer.{h,cpp}`, `Tests/Scene/USDLightMixer_Test.cpp`
  - approach: per group, dynamic knobs on a "Groups" page, created and rebuilt like P1.T3's sets: `groupName`, `includes`, `excludes` (PrimSelector patterns), `masterIntensity` (default 1), `masterExposure` (default 0), `tint` (default 1,1,1), plus a node-level `addGroup` button that adds a set. Author `collection:<name>` on `/collections` (defining the scope prim when absent) with the light members the patterns match and expansion rule `explicitOnly`. Use the collection helper in `SceneNodeAuthoring.cpp` (the `UsdCollectionAPI::Apply` path) where it fits. An invalid identifier for the name is an error. An empty match warns.
  - verify: gtest: a group including `/lights/key*` and excluding one light authors a collection holding exactly the expected members, survives a serialize and load round trip, and an empty match warns.
  - size: M

- [ ] M573.P2.T2 — Apply group masters to member lights
  - files: `Engine/Nodes/Scene/USDLightMixer.cpp`, `Tests/Scene/USDLightMixer_Test.cpp`
  - approach: for each member light, the effective intensity is (override or input) times the product of its groups' master intensities, exposure adds their master exposures, and color multiplies their tints per component. Author the result only when a master differs from its identity or the light has an override. Animated masters author time samples at the union of the times the knob and override have samples.
  - verify: gtest: a light in two groups with master intensities 2 and 0.5 and exposures 1 and 1 authors intensity = input, exposure = input + 2, and a tint applied over a color override multiplies it. Identity masters author nothing.
  - size: M

## Phase 573.3: Table

- [ ] M573.P3.T1 — The mixer table widget
  - files: `Gui/KnobGuiLightMixer.{h,cpp}` (new), `Gui/LightMixerModel.{h,cpp}` (new), `Gui/KnobGuiFactory.cpp`, `Gui/CMakeLists.txt`, `Engine/Nodes/Scene/USDLightMixer.{h,cpp}` (a public accessor for rows and override sets), `Tests/gui/m573_light_mixer.py` (new), GUI test registration
  - approach: a `KnobGui` built for the `mixerTable` placeholder (an `isMixerKnob` check in `createGuiForKnob`, beside `LoopDriverTable::isTableKnob`). It is a `QTableView` over a lazy `QAbstractItemModel`, with Path, Type and the In columns, the optional ones following the show toggles, and a filter line edit. While visible, input and knob changes run `collectLights` off the UI thread with `getShowingViewerContext` and `SceneQueryPool::borrow`, through `QtConcurrent` and a `QFutureWatcher`, with a generation counter dropping stale results. The old rows stay up, marked updating. Editing a cell calls `ensureLightOverrides` and sets the knob through its undoable command. Bool columns are checkboxes, numbers spin-sliders, and Color opens the regular colour picker. Edited cells are bold, missing rows greyed with the Remove Override action, and instance proxies read-only. A hidden widget computes nothing. It fits a 1920x1080 property panel.
  - verify: the Xvfb script builds two USDLights → USDMerge → USDLightMixer shown in a viewer, opens the panel, waits for two rows, edits intensity through the model and asserts the override knob value and that the cell is bold, undoes and asserts it is restored, toggles Solo and asserts the other light's output visibility, and saves a screenshot.
  - size: L

- [ ] M573.P3.T2 — Group rows and New Group
  - files: `Gui/KnobGuiLightMixer.cpp`, `Gui/LightMixerModel.cpp`, `Tests/gui/m573_light_mixer.py`
  - approach: the model becomes a tree. Each group is a collapsible parent row (collapsed by default) showing its master Intensity, Exposure and Tint in the matching columns, with its member lights under it. Ungrouped lights sit at the top level. A light in several groups appears under each. A "New Group" button makes a group from the selected light rows (includes = their paths) and names it `group<N>`. Editing a group row's cell sets its master knob.
  - verify: the Xvfb script selects both light rows, clicks New Group, asserts a group row with two children and the authored collection, sets the master intensity to 2 in the cell and asserts both lights' output intensity doubled, and saves a screenshot with the group expanded.
  - size: M

- [ ] M573.P3.T3 — Select lights in the viewer from rows
  - files: `Gui/KnobGuiLightMixer.cpp`, `Tests/gui/m573_light_mixer.py`
  - approach: selecting light rows sets the scene prim selection through the path M232 - Scene Selection To Nodes uses (`SceneViewport::setSelectedPrims` or its wrapper), so the viewer and Scene Graph panel highlight them. Group rows select their members. Missing rows select nothing.
  - verify: the Xvfb script selects one row and asserts the viewer's selected prims equal that light's path.
  - size: M

## Phase 573.4: Docs And Audit

- [ ] M573.P4.T1 — Document USDLightMixer
  - files: `Engine/Nodes/Scene/docs/USDLightMixer.md` (new), tooltips in `Engine/Nodes/Scene/USDLightMixer.cpp`
  - approach: in the style of `docs/USDLight.md`, cover what the table lists, what each column authors (Mute and Solo as visibility, Enable as `active` with its effect on children), override knobs and how they animate, groups and their master maths, missing overrides, instance proxies, and the unloaded-payload rule. Never name another application in the doc or tooltips.
  - verify: the doc renders in the node's help, and a grep of the doc and the `.cpp` for the reference applications' names finds nothing.
  - size: M

- [ ] M573.P4.T2 — Audit the reference controls against the knobs
  - files: `Engine/Nodes/Scene/USDLightMixer.cpp` (read), the PR body
  - approach: walk `.local/docs/houdini/nodes/lop/lightmixer.md` (UI, configuration keys, parameters, Transforms) and map every control to its knob or table column, or to its Out reason in this file's Scope. Fix any In control that is missing. Put the table in the PR body only.
  - verify: every reference control appears in the table with a knob, a column or an Out reason, and none listed In is missing.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDLightMixer_Test` and `USDLightMixerLights_Test` pass, and so do `USDRenderStorm_Test` and `USDRenderCycles_Test` with the muted-light cases; `Tests/gui/m573_light_mixer.py` passes under Xvfb at 1920x1080; the user OKs the table and group screenshots.

## Decisions

- 2026-11-24 — **A node, not a panel (user):** "Not a panel, a node". The light mixer is a USDLightMixer node whose settings panel embeds the light table and which authors its overrides in its own layer, like the Solaris Light Mixer LOP, not a graph-wide dockable editor.
- 2026-11-24 — **Mute and Solo hide by visibility, with an active fallback (user):** they author `visibility = invisible`. If P1.T5's probe finds a delegate that ignores visibility on lights, they author `active = false` instead.
- 2026-11-24 — **Sliders view Out (user):** the table is the only view.
- 2026-11-24 — **Light groups In (user):** groups author collections and their master intensity, exposure and tint apply to member lights, as in Phase 573.2.
