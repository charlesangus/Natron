## Pending


### 2026-11-24T17:20:48Z — change-request
- refs: M238
- The user answered M238 - EdgeExtend Options' Q1–Q3 (2026-11-24) and the milestone was grilled: replace the whole milestone file with the content below (the board name cell is already "EdgePush And Edge Fill Kit"), clear the row's Needs: response, and decide whether the row goes back to todo/doing per your claim. Project decision: PLAN/DECISIONS/2026-11-24-edge-extend-node-family.md. M591–M595 depend on it.

````markdown
# M238 - EdgePush And Edge Fill Kit

Revised 2026-11-24 after the user's prior-art discussion: instead of one node with many options, edge extension becomes a family of single-method nodes (EdgePush, EdgeClamp, EdgeSmear, EdgePushVector, EdgeDistort) plus a separate EdgeDetail node, all built on a shared CPU+GPU kit (`PLAN/DECISIONS/2026-11-24-edge-extend-node-family.md`). This milestone builds the kit and **EdgePush**, the push-pull pyramid fill. EdgePush replaces and renames the native EdgeExtend, with no compatibility. M591–M595 build the other nodes on the kit.

Reference: Nuke 17's EdgeExtend (`.local/docs/nuke/html/content/reference_guide/filter_nodes/edgeextend.html`) for the shared stages; push-pull is Gortler et al. 1996 (weighted pyramid: down, then up and merge finer over coarser).

## Scope

**In:**
- `EdgeFillKit` (CPU and GPU), shared by every fill node:
  - **Matte selection:** an optional Aux **Matte** input and a `matte` choice. IDs: none, sourceAlpha (default), sourceInvertedAlpha, matteLuminance, matteInvertedLuminance, matteAlpha, matteInvertedAlpha.
  - **Premultiply knobs:** `sourcePremultiplied` (default **off**) and `premultiplyOutput` (default **off**). Input and output are straight by default.
  - **Threshold and softness:** `threshold` and `softness` (default 0). They give a solid weight `w` that ramps from 0 to 1 across [threshold − softness/2, threshold + softness/2].
  - **Erode:** `erode`, fractional, render-scaled, negative values dilate. Applied to the matte before the threshold.
  - **Edge mask:** an `edgeMaskLayer` output (default none). When set, the node writes `alpha × (1 − w)` to that named layer alongside RGBA.
  - **Fill extent:** `fillExtent`, either Source RoD (default) or Source RoD And Format, the union of the source RoD and the project format.
- EdgePush itself: the kit, plus a push-pull fill with no reach limit. The output is `w·src + (1 − w)·fill` and the output alpha is the source's. Panel order: Matte, Source Premultiplied, Threshold, Softness, Erode, Fill Extent, Edge Mask Layer, Premultiply Output.

**Out:**
- **Detail restoration:** M595 - Native EdgeDetail.
- **Other fill methods:** M591 - Native EdgeClamp, M592 - Native EdgeSmear, M593 - Native EdgePushVector, M594 - Native EdgeDistort.
- **Size and Slices:** removed, no compatibility.
- **Reach-limit knob:** none. Reach is the fill extent.
- **(Un)premult-by knob:** none; this is a filter.
- **Rejected methods:** Poisson, Telea / Navier-Stokes, PatchMatch, Farbman kernels.

**Edge cases:**
- **`matte` = None:** every pixel is solid. Colour passes through apart from the premultiply round trip.
- **Matte option chosen, Matte input disconnected:** an empty matte, so a black fill, no error (EdgeBlur's rule).
- **Nothing solid** (erode past the whole matte, or a threshold above every pixel): replaced pixels are black (0, not NaN), and the render never fails (user decision).
- **Pixels outside the source RoD** under Source RoD And Format: source colour and alpha are 0. They get fill colour and an edge mask of 0.
- **Tiling:** push-pull needs the whole extent, so the RoI is the fill extent. A tiled render must match a whole-frame render bit-exact.
- **Luminance:** uses the working-space luma coefficients of the native colour nodes.
- **Edge mask layer:** the edge mask layer goes through the same output-layer mechanism as native Shuffle (`Shuffle::getOutputLayer` and `resolveOutputLayerDesc`, ChannelCopy's `args.outputLayers`).

**Assumptions:**
- Unconfirmed: `threshold` defaults to 1.0, the opaque core only. The user wanted it adjustable and did not pick a default.

## Phase 238.1: Kit and EdgePush on the CPU

- [ ] M238.P1.T1 — Rename the native EdgeExtend to EdgePush
  - files: `Engine/Nodes/Filter/EdgeExtend.{h,cpp,reg.inc}` → `EdgePush.*`, `Engine/Nodes/Filter/docs/EdgeExtend.md` → `EdgePush.md`, `Engine/Gpu/Kernels/edgeextend.{slang,cmake}` → `edgepush.*`, `Tests/Native/NativeEdgeExtend*_Test.cpp` → `NativeEdgePush*_Test.cpp`, `Tests/fixtures/native-golden/EdgeExtend/` → `EdgePush/`, CMake and test lists, `Engine/OfxRetired/README.md` (the retired `eu.cimg.EdgeExtend` now points at EdgePush)
  - approach: `git mv` the files, then rename the class, plug-in ID, label, kernel and test names. Add no alias for the old native ID. Make no behaviour change in this task. Add an "extend" search tag to the description.
  - verify: `tools/ci/local/syntax-check.py` on the touched TUs; at the phase build, `ctest -R NativeEdgePush` and `NativeGolden` pass unchanged.
  - size: M
- [ ] M238.P1.T2 — EdgeFillKit: matte selection, premultiply knobs, threshold and softness (CPU)
  - files: `Engine/Nodes/Filter/EdgeFillKit.{h,cpp}` (new), `Engine/Nodes/Filter/EdgePush.{h,cpp}`, `Tests/Native/NativeEdgeFillKit_Test.cpp` (new), CMake
  - approach: the kit defines the shared knobs and the optional Aux "Matte" input, read the way EdgeBlur reads its input (`EdgeBlur.h:52`, `MapInput.h`). It computes the solid weight `w` per the Scope, with `sourcePremultiplied` and `premultiplyOutput` both defaulting off. EdgePush calls the kit in place of its own threshold code, and its current slice fill stays until T4. Keep the API usable by M591–M595: a params struct, a CPU function that computes the weight image, and knob definition and readback helpers.
  - verify: gtests for each `matte` option on a synthetic matte, softness 0 vs a ramp, disconnected Matte giving an empty matte, `matte` None as identity, and the defaults.
  - size: M
- [ ] M238.P1.T3 — EdgeFillKit: erode (CPU)
  - files: `Engine/Nodes/Filter/EdgeFillKit.{h,cpp}`, `Tests/Native/NativeEdgeFillKit_Test.cpp`
  - approach: apply IErodeDilate's fractional kernel to the chosen matte before the threshold, render-scaled. The RoI grows by |erode|.
  - verify: erode shrinks the solid region, a negative value grows it, eroding past the matte leaves nothing solid, and 0 is the identity.
  - size: M
- [ ] M238.P1.T4 — Push-pull fill and fill extent replace the slices (CPU)
  - files: `Engine/Nodes/Filter/EdgePush.{h,cpp}`, `Engine/Nodes/Filter/EdgeFillKit.{h,cpp}` (fill extent), `Engine/Nodes/Filter/BloomKernels.h`, `Tests/Native/NativeEdgePush_Test.cpp`
  - approach: remove `size`, `slices`, `sliceSigmas` and `kMaxSlices`. Build a weighted pyramid of (straight colour·w, w) with [1 3 3 1] decimation, reusing Bloom's helpers, down until no level has a zero-weight pixel or it reaches 1 px. Come back up with bicubic upsampling, merge each finer level over the coarser one (`a + b(1 − αa)`), and normalise. The output is `w·src + (1 − w)·fill`; if nothing is solid, the fill is 0. The kit's `fillExtent` sets the output RoD and the RoI, which is the whole extent.
  - verify: solid pixels are unchanged; a pixel 500 px from the matte gets the nearest edge colour; nothing solid gives black with no NaN; RoD And Format fills outside the source RoD; a tiled render equals a whole-frame render bit-exact.
  - size: L
- [ ] M238.P1.T5 — Edge mask layer output (CPU)
  - files: `Engine/Nodes/Filter/EdgeFillKit.{h,cpp}`, `Engine/Nodes/Filter/EdgePush.{h,cpp}`, `Tests/Native/NativeEdgePush_Test.cpp`
  - approach: add an `edgeMaskLayer` layer choice: none (default), an existing layer, or a new one, the way Shuffle's output layer works (`Shuffle::getOutputLayer` and `resolveOutputLayerDesc`). When set, the node also produces that layer, filled with `alpha × (1 − w)`. RGBA is unchanged.
  - verify: the mask of a known disc matches analytically in a named layer; with none, no extra layer and RGBA bit-identical to the run with the layer set.
  - size: M

## Phase 238.2: GPU

- [ ] M238.P2.T1 — EdgeFillKit GPU stages
  - files: `Engine/Gpu/Kernels/edgefillkit.{slang,cmake}` (new), `Engine/Nodes/Filter/EdgeFillKit.cpp`, `Tests/Native/NativeEdgeFillKitGpu_Test.cpp` (new)
  - approach: entries for matte selection, the solid weight and the edge mask; erode goes through `ierodedilate.slang`. It is a module the other fill nodes' kernels import.
  - verify: `ctest -R NativeEdgeFillKitGpu` within 1e-4 relative on RADV for every `matte` option, erode in both directions and softness.
  - size: L
- [ ] M238.P2.T2 — EdgePush push-pull on the GPU
  - files: `Engine/Gpu/Kernels/edgepush.{slang,cmake}`, `Engine/Nodes/Filter/EdgePush.cpp`, `Tests/Native/NativeEdgePushGpu_Test.cpp`
  - approach: pyramid-down and pyramid-up/merge entries. The pyramid stays resident, with no CPU fallback. Update gpuWeight.
  - verify: `ctest -R NativeEdgePushGpu` within 1e-4 relative on RADV, including both fill extents and the edge mask layer.
  - size: L

## Phase 238.3: Goldens, docs and sign-off

- [ ] M238.P3.T1 — Goldens, description and docs
  - files: `Tests/Native/NativeGolden_Test.cpp`, `Tests/fixtures/native-golden/EdgePush/`, `EdgePush.h` description, `Engine/Nodes/Filter/docs/EdgePush.md`, `Engine/Nodes/docs/edge-policy.md` if it names EdgeExtend
  - approach: replace the oneslice and wide cases with matte, erode, softness, fill extent and edge mask cases, then re-record them (≤256 px crops). Rewrite the doc and description, naming every knob and the kit stages.
  - verify: `ctest -R NativeGolden`; every knob is named in the doc.
  - size: M
- [ ] M238.P3.T2 — Xvfb GUI check
  - files: `Tests/gui/m238_edgepush.py`
  - approach: at 1920x1080, create EdgePush, connect a Matte, cycle the `matte` options, erode in both directions, softness 0 and 0.2, both fill extents, and the edge mask layer set to `edge.mask`. The panel must fit without scrolling. Take screenshots.
  - verify: passes on the remote GUI runner.
  - size: M

**Verification gate:** remote `release --tests` green, including `NativeEdgeFillKit*`, `NativeEdgePush*` and `NativeGolden`; GPU parity on RADV; the GUI check passes remotely; screenshots approved; an AppImage for UAT.

## Decisions

- 2026-10-28: elaborated (consultant); blocked on Q1–Q3 before implementation.
- 2026-11-24 — Split into a node family on a shared kit (user): single-method nodes avoid an opaque uber node. This milestone is the kit plus EdgePush; M591–M595 are the rest. Resolves Q1.
- 2026-11-24 — Push-pull replaces Size and Slices, no compatibility (user). EdgeExtend is renamed EdgePush. Resolves Q2.
- 2026-11-24 — Edge mask goes to a named output layer, `alpha × (1 − w)` (user): native Shuffle and ChannelCopy already produce named layers, so the E14 stub is unnecessary. Resolves Q3.
- 2026-11-24 — Input and output are straight by default (user): `sourcePremultiplied` and `premultiplyOutput` both default off.
- 2026-11-24 — Softness blends `w·src + (1 − w)·fill` (user), so there is no hard line at the threshold and noisy keys stay steadier.
- 2026-11-24 — Nothing solid gives a black fill and never an error (user).
- 2026-11-24 — Fill extent is the source RoD by default, with an option for the union of the source RoD and the format (user). There is no separate reach limit.
- 2026-11-24 — Detail restoration is the separate M595 - Native EdgeDetail node (user), not knobs here.
````

### 2026-11-25T04:17:08Z — change-request
- refs: M247
- New milestone after M247 - Project User Knobs And Root (E10, F16 - User Knob Editor or standalone): **Remove The Old User Knob Dialogs** — the former M373.P3.T3. Delete Gui/ManageUserParamsDialog.{h,cpp}, Gui/AddKnobDialog.{h,cpp}, Gui/PickKnobDialog.{h,cpp} and every reference (includes, actions, Gui/CMakeLists.txt, DockablePanel.cpp, ProjectGui::editVariables). Anything still used — validation, the path-variable rename, PickKnobDialog's node listing — must already live in M372's API, the in-panel editor (M373) or M247's project editor; move leftovers rather than keeping a dialog. Verify: build; `git grep -n "ManageUserParamsDialog\|AddKnobDialog\|PickKnobDialog"` finds nothing; all m373_*.py GUI scripts pass. Blocked on M247 because the dialog still edits project path variables until M247 gives the project an in-panel editor.
