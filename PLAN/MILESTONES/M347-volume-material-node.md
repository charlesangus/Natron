# M347 - Volume Material Node

A new `USDVolumeMaterial` node with one set of volume controls, translated into each renderer's own shader on a single `UsdShadeMaterial`, and assigned through the existing `USDAssignMaterial`. Today a volume look needs a hand-written Cycles shader and a separate MoonRay shader, while Storm only draws its grey fallback.

## Scope

**In:**
- Shared controls: Density, Colour, Absorption Colour, Anisotropy, Emission Strength and Emission Colour, plus grid names for density and emission.
- Cycles output through `outputs:cycles:volume` with `cycles_principled_volume`.
- MoonRay output through `outputs:moonray:volume` with `BaseVolume`.
- Storm approximation of density, colour and emission through a volume shader.
- Node docs and icon, plus a table of how each control maps per renderer in the milestone report.

**Out:**
- Volume controls on `USDMaterial` — none (user chose a separate node, 2026-11-05).
- Surface shading on the same node — none.
- Temperature and blackbody controls — none (a future named milestone: MoonRay's geometry has no temperature grid).
- A node-graph shader editor or MaterialX volumes — none (OSL and MaterialX volume shading aren't in the bundled delegates).
- Delegate rendering fixes — M345 - VDB Rendering Fixes.

**Edge cases:**
- Material assigned to a non-volume prim: renderers ignore the volume output, and M345 - VDB Rendering Fixes' surface check still reports the missing surface.
- Density grid name not bound on the volume: raises the existing grid warning for the renderer concerned.
- A control a renderer can't honour exactly: use the nearest mapping and record it in the control's tooltip.
- Volume with a surface material also assigned: out of this node's hands; `USDAssignMaterial`'s single binding applies.

**Assumptions:**
- Unconfirmed: Storm accepts a custom volume shader through a `glslfx` volume output and `HdSt`'s volume shader API. M347.P1.T1 confirms it from `pxr` sources and the bundled `fallbackVolume.glslfx`.
- Unconfirmed: hdMoonray reads `outputs:moonray:volume` through its current render contexts. The audit saw a bound `BaseVolume` change the render's colour, though M325 - MoonRay Fork notes it doesn't override `GetMaterialRenderContexts`. Confirm which path carried it.

## Phase 347.1: Research

- [ ] M347.P1.T1 — Confirm the three shading routes
  - files: milestone `## Decisions` only
  - approach: Read `pxr/imaging/hdSt` volume sources and `fallbackVolume.glslfx`, `Cycles` `PrincipledVolumeNode` sockets (`src/scene/shader_nodes.cpp`) and the bundled `moonray/coredata/BaseVolume.json`. Check `.local/docs/solaris`, `.local/docs/katana` and `.local/docs/gaffer` for how others expose these controls. Record the control-to-parameter table and any Storm limit. If Storm can't take a custom volume shader, put it to the user before building.
  - verify: The table covers every control for every renderer, and each entry cites the source it came from.
  - size: M

## Phase 347.2: Node

- [ ] M347.P2.T1 — Add USDVolumeMaterial with Cycles output
  - files: `Engine/Nodes/Scene/USDVolumeMaterial.h`, `Engine/Nodes/Scene/USDVolumeMaterial.cpp`, `Engine/Nodes/Scene/USDVolumeMaterial.reg.inc`, `Engine/CMakeLists.txt`, `Tests/Scene/USDVolumeMaterial_Test.cpp`
  - approach: Follow `USDMaterial` for structure and knob helpers. Author a `UsdShadeMaterial` with a `cycles_principled_volume` shader; the terminal output name must be `outputs:Volume` on the shader (`material.cpp` looks up the socket by that name, and `outputs:volume` fails with "Could not find terminal output"). Labels match script names. No (un)premult knob, since this isn't a colour node.
  - verify: The test asserts the authored network, and a Cycles render of the fixture changes as Density and Colour change.
  - size: L
- [ ] M347.P2.T2 — Add the MoonRay output
  - files: `Engine/Nodes/Scene/USDVolumeMaterial.cpp`, `Tests/Scene/USDVolumeMaterial_Test.cpp`
  - approach: On the same material, author a `BaseVolume` shader under `outputs:moonray:volume`, mapped per the T1 table.
  - verify: The test asserts the network, and a MoonRay fixture render changes as Density and Colour change.
  - size: M
- [ ] M347.P2.T3 — Add the Storm approximation
  - files: `Engine/Nodes/Scene/USDVolumeMaterial.cpp`, `Engine/Nodes/Scene/Resources/` (the shader file), `Tests/Scene/USDVolumeMaterial_Test.cpp`
  - approach: Author the Storm route confirmed by T1 for density, colour and emission. It approximates: it needn't match path-traced lighting, only read as the same look. Keep Storm's fallback working when the material isn't bound.
  - verify: A Storm fixture render's colour and thickness follow the knobs, under the GL test recipe.
  - size: L

## Phase 347.3: Delivery

- [ ] M347.P3.T1 — Node docs and icon
  - files: `Engine/Nodes/Scene/docs/USDVolumeMaterial.md`, `Gui/Resources/Images/NativeNodes/USDVolumeMaterial.png`, `Gui/GuiResources.qrc`
  - approach: Document each control and where a renderer approximates it. No Nuke references.
  - verify: The docs build lists the node, and the icon appears in the node menu.
  - size: M
- [ ] M347.P3.T2 — GUI check of the panel and look
  - files: `Tests/gui/usd_volume_material.py`
  - approach: Build `USDVolume -> USDAssignMaterial` with `USDVolumeMaterial` under the GUI harness, change Colour and Density, and render with each renderer. Shoot the panel and each render, one file per screenshot.
  - verify: The script passes through `Tests/gui/run-gui-test.sh`, and the screenshots go to the user for sign-off.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`, including `USDVolumeMaterial`. On the AppImage, one material node changes a volume's colour and density in Storm, Cycles and MoonRay; panel and render screenshots approved by the user.

## Decisions

- 2026-11-05 — Dedicated `USDVolumeMaterial` node, assigned through `USDAssignMaterial` (user): keeps `USDMaterial` simple, and shows only the controls that apply.
- 2026-11-05 — Storm approximates density, colour and emission (user): the viewer preview should resemble the final look; lighting won't match a path tracer.
- 2026-11-05 — The 2026-11-05 audit's MoonRay test used `BaseVolume` under `outputs:moonray:volume`, and its Cycles test uses `cycles_principled_volume` under `outputs:cycles:volume`, with `MaterialBindingAPI` applied on the volume prim: without that schema USD warns about the binding and ignores it.
