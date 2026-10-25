# M184 - Variable-Size Filter Kit

The shared base for the exact native I-nodes (user, 2026-10-24: separate registrations sharing each base node's class, plus a Make I-Node action), and its first three nodes: **IBlur**, **IErode** and **IDilate**. Each I-node takes a **Map** input (user's name) whose value scales the effect per pixel: Map black gives the node's size-at-black (default 0, so no effect), Map white its normal size, with a smooth transition between — an eye blur that is sharp where the Map is black. The standard host Mask and Mix stay as well, so the result can still be limited. A node opts out of the host blend for the Map by declaring it as an aux input, the way STMap reads its UV input (`STMap.cpp:464` on M113's branch); the host Mask keeps `Stage::MaskMix` (`Engine/Nodes/Image/ImagePipelineSpec.h:39`).

## Scope

**In:**
- `MapInput`: the shared Map convention — an input named Map, a layer/channel knob (`Node::declareLayerKnob`), `mapInvert`, `mapBlack` and `mapWhite` remap points, a `sizeAtBlack` knob paired with the node's own size knob (the size at white), read into a float plane; plus per-tile min/max of the remapped Map.
- An I-variant registry: base plug-in ID → I-variant ID, with the knob mapping used to convert one into the other (M189 - Make I-Node Action reads it).
- IBlur (Gaussian-like, exact per-pixel radius), IErode and IDilate (per-pixel radius, square and disc shapes), each with CPU and GPU kernels, tests, cost seeds, icons and docs.

**Out:**
- IDefocus: M186 - Exact IDefocus. Warps: M185 - Variable Warps. ISharpen/IGlow: M187 - Variable Sharpen And Glow. IMedian/IDirBlur: M188 - Variable Median And DirBlur. Radial and zoom blur come as IDirBlur modes in M188 - Variable Median And DirBlur. An EdgeBlur-style soften: no milestone owns it.
- Changing the base Blur, Erode or Dilate nodes.

**Edge cases:**
- Map not connected: the node behaves as its base node at full size, with a warning badge.
- Map 0 at a pixel with `sizeAtBlack` 0: output equals input exactly at that pixel (no filter ringing). With a non-zero `sizeAtBlack`, Map 0 is the effect at that size.
- Radius 0 everywhere in a tile: the tile is a pass-through copy.
- RoI expands by the larger of the two sizes, since a tile's Map is not known in the request pass.
- Proxy/render scale: sizes scale with render scale like the base nodes.

## Phase 184.1: Map convention and registry

- [x] M184.P1.T1 — Write the shared MapInput helper
  - files: `Engine/Nodes/Filter/MapInput.h`, `Engine/Nodes/Filter/MapInput.cpp` (new), `Tests/Native/MapInput_Test.cpp`
  - approach: Declares the Map input (aux role, optional) and its knobs on a node spec; reads the chosen channel of the chosen layer for a RoI into a float plane, applying invert and the black/white remap and clamping to [0,1], and maps it to a size with `sizeAtBlack` and the node's size; outside the Map's RoD the value is 0. `tileRange(rect)` returns min/max. Model the layer knob on STMap's `kSTMapParamChannelU`.
  - verify: at the phase build, `ctest -R MapInput` passes: remap, invert, clamp, outside-RoD and min/max cases.
  - size: M

- [x] M184.P1.T2 — Add the I-variant registry
  - files: `Engine/Nodes/IVariantRegistry.h`, `Engine/Nodes/IVariantRegistry.cpp` (new), `Engine/Nodes/README.md`
  - approach: A static table registered from each I-node's `.reg.inc` (a second macro, `NATRON_I_VARIANT(baseId, iId, knobMap)`), where `knobMap` lists base knob → I knob names; base knobs absent from the map copy by name. Document it in the native node checklist.
  - verify: gtest: lookups both ways, and a missing entry returns none.
  - size: M

## Phase 184.2: IBlur

- [x] M184.P2.T1 — Write the native IBlur node (CPU)
  - files: `Engine/Nodes/Filter/IBlur.h`, `Engine/Nodes/Filter/IBlur.cpp`, `Engine/Nodes/Filter/IBlur.reg.inc`, `Engine/Nodes/Filter/VariableBlurKernels.{h,cpp}`
  - approach: Per-pixel radius `r = lerp(sizeAtBlack, size, Map)`. Compute a summed-area table per channel per tile (double precision, tile-local origin) and apply a variable-width box three times (iterated boxes approximate a Gaussian of sigma matching the base Blur's `sizeToSigma`), each pass reading the per-pixel radius, so cost is O(1) per pixel at any size. Knobs mirror Blur's (`size` 2-D, channels, crop to format) plus `MapInput`'s.
  - verify: at the phase build, `ctest -R NativeIBlur` (T2) passes.
  - size: L

- [x] M184.P2.T2 — Test IBlur on CPU
  - files: `Tests/Native/NativeIBlur_Test.cpp`
  - approach: Map 0 is bit-identical to the input. Map 1 matches base Blur at the same size within 2% RMS (the box-iterated kernel differs from the FIR Gaussian). A half-black Map leaves the black half untouched beyond 0 px. A constant input stays constant. Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeIBlur` passes.
  - size: M

- [x] M184.P2.T3 — IBlur on the GPU
  - files: `Engine/Gpu/Kernels/iblur.slang`, `Engine/Gpu/Kernels/iblur.cmake`, `Engine/Nodes/Filter/IBlur.cpp`, `Tests/Native/NativeIBlurGpu_Test.cpp`
  - approach: Prefix-scan SATs per tile (two passes), then the three variable-box passes; refuse above a size cap and fall back to CPU.
  - verify: at the phase build, `ctest -R NativeIBlurGpu` passes on lavapipe within 1e-4 relative.
  - size: L

## Phase 184.3: IErode and IDilate

- [x] M184.P3.T1 — Write the native IErode and IDilate nodes (CPU)
  - files: `Engine/Nodes/Filter/IErodeDilate.h`, `Engine/Nodes/Filter/IErodeDilate.cpp`, `Engine/Nodes/Filter/IErode.reg.inc`, `Engine/Nodes/Filter/IDilate.reg.inc`
  - approach: One templated class, two registrations, mirroring `ErodeDilateNode<ErodeDilateMode>`. Per-pixel radius `r = lerp(sizeAtBlack, size, Map)`. Build a per-row sparse table (range min or max) per tile; each output pixel queries one range per row within its radius (square: full width; disc: the row's half-width), so cost is O(r) per pixel. Fractional radii blend between the floor and ceil results for a smooth transition.
  - verify: at the phase build, `ctest -R NativeIErodeDilate` (T2) passes.
  - size: L

- [x] M184.P3.T2 — Test IErode and IDilate on CPU
  - files: `Tests/Native/NativeIErodeDilate_Test.cpp`
  - approach: Map 0 is identity. Map 1 with the square shape is bit-identical to base Erode/Dilate at the same integer size. The disc shape matches a brute-force disc min/max. A Map ramp gives a monotonic result across the ramp. Record regression fixtures.
  - verify: at the phase build, `ctest -R NativeIErodeDilate` passes.
  - size: M

- [x] M184.P3.T3 — IErode and IDilate on the GPU
  - files: `Engine/Gpu/Kernels/ierodedilate.slang`, `Engine/Gpu/Kernels/ierodedilate.cmake`, `Engine/Nodes/Filter/IErodeDilate.cpp`, `Tests/Native/NativeIErodeDilateGpu_Test.cpp`
  - approach: Sparse-table build in shared memory per tile row band, then the per-pixel range queries.
  - verify: at the phase build, `ctest -R NativeIErodeDilateGpu` passes on lavapipe, bit-exact with the CPU.
  - size: L

## Phase 184.4: Finish

- [ ] M184.P4.T1 — Register the I-variants and cost seeds
  - files: `Engine/Nodes/Filter/IBlur.reg.inc`, `Engine/Nodes/Filter/IErode.reg.inc`, `Engine/Nodes/Filter/IDilate.reg.inc`, `Engine/Nodes/Filter/{IBlur,IErode,IDilate}.costseed.inc`
  - approach: Add the `NATRON_I_VARIANT` lines (Blur → IBlur, Erode → IErode, Dilate → IDilate, with their knob maps) and cost seeds, per `Engine/Nodes/README.md`'s checklist.
  - verify: `ctest -R NativePluginList` and the registry gtest pass with the three entries.
  - size: S

- [x] M184.P4.T2 — Icons and docs
  - files: `Gui/Resources/Images/NativeNodes/`, `Engine/Nodes/Filter/docs/{IBlur,IErode,IDilate}.md`
  - approach: Each doc states the Map convention, the algorithm and the `Tolerance:` line.
  - verify: docs reviewed at the gate against the node code.
  - size: S

## Decisions

- 2026-10-25 — **UAT approved (user):** the gate screenshots and the AppImage with the Map remap controls are signed off.
- 2026-10-25 — **Map remapping control; no Map Black/White (user, UAT):** the Map input gets a simple remap: a Smooth/Linear choice and a gamma applied to the map. The Map Black and Map White knobs are removed. Size At Black comes before Size in the panel. IErode and IDilate fold into one IErodeDilate in M111 - Native Node Identity Cleanup, alongside ErodeDilate.
- 2026-10-25 — **Gate status (bosley-native-nodes):** release 453/453; `m184_variable_filters.py` 0 failures; eye-blur, IErode/IDilate and icon screenshots shared, awaiting approval. Cost seeds are held back: the CPU profiles ran at load 9-16 on 4 cores (IBlur 835, IErode 1261, IDilate 1459 ms/Mpx serial — likely inflated) and the other nodes' profile inputs no longer exist on this host, so the regenerate command can't be reproduced. Re-measure on a quiet host, and the GPU rows and RADV parity need the GPU host.
- 2026-10-25 — **IErode/IDilate calls (bosley-native-nodes):** one template drives both (plain IDs `IErode`, `IDilate`); sizes are 0 or more, so M189's converter turns a negative base size into the opposite node and can't convert a base with one axis positive and one negative. GPU tables live in global memory per channel, not shared-memory tiles; radii over 255 px stay on the CPU (keeps the integer ellipse maths in 32 bits). White Map + square matches ErodeDilate bit for bit except a base-node quirk on lines of length halfWidth+2 and the sign of zero. Radii are not divided by pixel aspect, as in the base node.
- 2026-10-25 — **IBlur GPU precision and cap (bosley-native-nodes):** the GPU summed-area table sums each plane exactly in 64-bit integers after a power-of-two scale, so it meets 1e-4 relative without fp64; halos over 512 px stay on the CPU (reachable through Size At Black, since Size stops at 1000). Native IDs are plain (`IBlur`). Fast build 436/436.
- 2026-10-25 — **IBlur GPU through the pass runner (bosley-native-nodes):** the GPU pass runner gains views of extra (Aux) inputs (getGpuInputLayer/getGpuInputRoI cover them), reusable by STMap and the kit, rather than a custom renderOpGpu for IBlur. `Pass::columns` lets a pass run over columns. Golden fixtures are recorded from the phase build tree with NATRON_GOLDEN_RECORD_DIR.
- 2026-10-25 — **Outside the Map (bosley-native-nodes):** outside the Map image the raw value is 0 (black) and invert/remap still apply, as for any black pixel; an inverted roto Map therefore gives full effect outside its area.
- 2026-10-25 — **Fragment macro (bosley-native-nodes):** `NATRON_I_VARIANT(BASE_ID, I_ID, {"base","i"}, ...)` takes a variable list of knob pairs; the generated `NativeRegistryAll.inc` defines both macros empty when an includer hasn't. STMap's Map-knob model lives on the M113 branch until it merges.
- 2026-10-25 — **Separate I-variant registrations plus Make I-Node (user):** base nodes stay lean and keep concatenating; the registry lets M189 - Make I-Node Action convert either way.
- 2026-10-25 — **Input named Map (user).**
- 2026-10-25 — **Iterated variable box for IBlur (user):** O(1) per pixel at any size; ~2% RMS from Blur at full size is accepted.
- 2026-10-25 — **Size at black and at white (user):** transitions need not start from zero.

**Verification gate:** the full build and ctest pass, including every new CPU and GPU suite on lavapipe; GPU parity also passes on RADV. Under Xvfb an eye blur (IBlur with a roto Map), a Map-driven IErode and IDilate on a key are rendered; screenshots shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.
