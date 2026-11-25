# M458 - ColorWarp Node

Second member of F28 - Colour Warp. A native `ColorWarp` colour node built on M457 - ColorWarp Space Spike's `ColorWarpField`: the user clicks in the viewer to drop a pin on a colour, pushes the pin's target with the existing colour trackball and its global mode (M44 - Trackball Colour Editing, Ctrl+E: C/S/T/V drags in the CIE xy chroma plane on the last-touched colour knob), and builds up a complex warp as pins are added. Point-op structure and GPU kernel follow `ColorLookup` (`Engine/Nodes/Color/ColorLookup.{h,cpp}`, `ImageNode<Op>`, `Engine/Gpu/Kernels/colorlookup.cmake`); viewer overlay and item serialisation follow SplineWarp (`Engine/Nodes/Transform/SplineWarp/`, `SplineWarpInteract`, `Serialization/SplineWarpSerialization.h`); per-pin knob holders follow `TrackMarker` (`Engine/TrackMarker.h`).

## Scope

**In:**
- Pins: each has Source colour, Target colour (a real colour knob, so the trackball panel and global mode work on it), Strength (0-1), Falloff (reach in the warp space), Enabled, and its pick position and frame (for display). Target, Strength and Falloff are keyable. A pin with no shift is a hold pin that protects its colour.
- Node knobs: Warp Space and Kernel only if M457's decision keeps them as options, plus the standard Mix and mask.
- Panel: a pin list (add, delete, enable, rename) with the selected pin's knobs below it.
- Viewer: click empty space adds a pin at that pixel with its source sampled from the node's **input** at the current frame (target = source); pins drawn at their pick positions with source/target swatches; dragging a pin slides it to another pixel and re-samples its source live, keeping its shift as an offset; Delete removes the selected pin; selecting a pin makes its Target the global mode's last-touched colour knob.
- CPU and GPU (Slang) evaluation of the same field, with parity tests.
- Node docs page.

**Out:**
- The 3D colour-space view — M459 - ColorWarp 3D View.
- LUT/.cube export — none (user, 2026-11-16: not now).
- Spatial (image-position) falloff: the warp is a pure colour op — none.

**Edge cases:**
- No pins / all disabled: bit-exact pass-through.
- Pick outside the input's RoD: no pin is added.
- Pins on an animated plate: the source is sampled at the pick frame and stays fixed until the pin is dragged again; pins draw at their pick position on every frame.
- Many pins: no cap; the field is re-prepared only when a pin knob changes (hash of pin values at the render time).
- HDR and negative inputs: pass through the field's conversions unclamped.

## Phase 458.1: Node and pins

- [x] M458.P1.T1 — ColorWarp node with pin items
  - files: `Engine/Nodes/Color/ColorWarp/ColorWarp.{h,cpp,reg.inc}` (new), `Engine/Nodes/Color/ColorWarp/ColorWarpPin.{h,cpp}` (new), `Serialization/ColorWarpSerialization.h` (new), `Engine/CMakeLists.txt`, `Tests/Native/NativeColorWarp_Test.cpp` (new)
  - approach: a native point-op node (`ImageNode<ColorWarpOp>` pattern) whose op holds a prepared `ColorWarpField` built from the enabled pins at the render time; pins are `KnobHolder` items like `TrackMarker`, each with the knobs above (labels match script names: `sourceColor` "Source Color", `targetColor` "Target Color", `strength`, `falloff`, `enabled`), owned by the node and saved/loaded through its own serialisation like SplineWarp's curves. Python access: `node.getPins()` style accessors only if the item model already provides them for Tracker; otherwise out.
  - verify: gtest — a node with one pin maps a chart patch's colour to the target; two pins and a hold pin behave as `ColorWarpField` does; save/load round-trips pins and keys; no pins is bit-exact pass-through.
  - size: L

- [x] M458.P1.T2 — Pin list in the panel
  - files: `Gui/ColorWarpPanel.{h,cpp}` (new), `Gui/CMakeLists.txt`, `Gui/DockablePanel.cpp` or the node's custom-panel hook (follow how `RotoPanel`/`TrackerPanel` are attached)
  - approach: a list of pins (name, enabled checkbox, source/target swatches) with add/delete buttons; selecting a row shows that pin's knobs below the list and tells the overlay; selection syncs both ways with the viewer.
  - verify: syntax-check; P3.T2's GUI script.
  - size: M

## Phase 458.2: Render

- [x] M458.P2.T1 — GPU kernel with parity
  - files: `Engine/Gpu/Kernels/colorwarp.slang` (new), `Engine/Gpu/Kernels/colorwarp.cmake` (new), `Engine/Nodes/Color/ColorWarp/ColorWarp.cpp`, `Tests/Native/NativeColorWarpGpu_Test.cpp` (new)
  - approach: mirror `colorlookup`'s kernel and params pattern; upload the prepared field's flat arrays (pin positions, displacements or RBF weights, affine part, space conversion matrices) as a buffer; evaluate per pixel with the same maths as the CPU path.
  - verify: GPU vs CPU within the usual parity tolerance on the chart with the three M457 pin sets.
  - size: L

## Phase 458.3: Viewer

- [x] M458.P3.T1 — Sample the input at a canvas position
  - files: `Engine/EffectInstance.h`, `Engine/EffectInstance.cpp` (or a small helper beside the overlay code), `Tests/Native/NativeColorWarp_Test.cpp`
  - approach: a helper that returns the node's input pixel value at a canvas position and time by requesting a 1×1 RoI render of the input through the normal render path (cache-friendly), usable from an overlay pen event; returns nothing outside the RoD. No native node can do this today (`setInteractColourPicker` is a no-op for native nodes).
  - verify: gtest — on a known constant/ramp input the helper returns the expected value; outside the RoD returns none.
  - size: M

- [x] M458.P3.T2 — Pin overlay, picking and global mode
  - files: `Engine/Nodes/Color/ColorWarp/ColorWarpInteract.{h,cpp}` (new), `Engine/Nodes/Color/ColorWarp/ColorWarp.cpp`, `Gui/ColorTrackballGlobalMode.cpp` (only if a hook is needed to set the last-touched knob), `Tests/gui/m458_colorwarp_pins.py` (new)
  - approach: overlay hooks like SplineWarp's: draw pins at their pick positions (source and target swatches, selected highlight); click empty adds a pin with P3.T1's sample; drag re-picks the source live and keeps target − source; Delete removes; selecting a pin makes its `targetColor` the global mode's last-touched colour knob so Ctrl+E drags push it. One undo step per add, move, delete. GUI script: add three pins on a chart, push one with global mode C, drag another to a new patch, delete the third, undo/redo each; check the output pixels and screenshot.
  - verify: the script passes in the remote gate.
  - size: L

## Phase 458.4: Docs

- [x] M458.P4.T1 — Node docs
  - files: `Engine/Nodes/Color/ColorWarp/docs/ColorWarp.md` (new), wherever native node docs are registered (follow `ColorLookup`'s)
  - approach: describe pins, hold pins, falloff, strength, the viewer workflow and global mode; no third-party product names.
  - verify: the docs build picks the page up.
  - size: S

**Verification gate:** build and ctest green, CPU/GPU parity green; the GUI script passes under the remote gate; member gate only — screenshots and UAT are collected at F28 - Colour Warp's UAT.

## Decisions

- 2026-11-16 — Pin targets are pushed with the existing trackball global mode (user: "reuse the global mode colour interaction"); targets are real colour knobs so it needs no new gesture.
- 2026-11-16 — Dragging a pin re-picks its source and keeps its shift (user).
- 2026-11-16 — Per-pin Strength, Falloff and hold pins, all keyable (user).
- 2026-11-16 — No LUT export in this milestone (user).
- 2026-11-19 (user, after the spike images and report at build/colorwarp-compare/REPORT.md in the M457 worktree): ColorWarp exposes every space and an RBF kernel option for the user to try, and the choices may be cut before shipping. Spaces: OKLab-log offered in both forms, log2 in place of the cube root (linear toe below 2^-7) and the plain cube root, plus Log RGB, XYZ and linear RGB. Kernels: the exact-at-pin Gaussian, and RBF (r, r³) as an option. Default space and kernel: OKLab-log (log2) with the Gaussian, as recommended. A global Lightness Falloff knob stretches a pin's reach along lightness (elliptical reach); its default keeps the ±1 stop reach. Strength 0 makes a hold pin. Each space's falloff is normalised (calibrated 0.93, 1.81, 0.11, 0.22 in the spike) so the same knob value reaches about as far in every space.
- 2026-11-23 — **Pins are colour mappings (user):** "I did not decide they should be real colour knobs, that is a misunderstanding"; "there is nothing tracker-like either. A pin is a mapping from one colour to another, with a strength." Targets are pushed with the global mode gesture (C/S/T/V chroma-plane drags) aimed at the selected pin directly. So: no per-pin KnobHolder items and no per-pin colour knobs; the pin set is node data (a pin-list knob holding source, target, strength and the other per-pin values, keyable where the 2026-11-16 decision says), and the global mode gains a hook to drive a selected pin target. Supersedes the P1.T1 approach and the "real colour knob" wording.
- 2026-11-24 — Outside the pins the warp fades to identity with distance (every kernel), so far-out colours pass through unchanged instead of extrapolating (user).
