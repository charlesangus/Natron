# M43 - Drop Premult Concept

**Revived 2026-10-24.** M43 was cancelled on 2026-09-19 because M38 - Layer Channel Widget's Phase 38.2 was meant to deliver it. On 2026-10-24 the user said "nodes are not supposed to be holding premult information either, that was supposed to be ripped out", so the milestone comes back. A survey of `origin/main` (`61b6976b6`) shows 38.2 did most of the Engine work. `ImagePremultiplicationEnum`, `getPremult`, `outputPremult` in `NodeMetadata`, the premult warning and the viewer premult paths were all deleted in `30c1f0ceb` (M38, #30). No Python binding, serialization or `NodeMetadata` field carries premult any more; the inbox report that named `ImagePremultiplicationEnum` and `getOutputPremultiplication` in Engine was reading stale code. What is left:

- **OFX host library (`libs/OpenFX` HostSupport, pinned `646ab00c`):** it still stores and derives premult. `_outputPreMultiplication` and `getOutputPreMultiplication()` are at `ofxhImageEffect.h:439, 487`. The value is derived in `setDefaultClipPreferences` (`ofxhImageEffect.cpp:2647-2751`) and read back from the plugin at `:2947`. The clip property defaults to `kOfxImageOpaque` (`ofxhClip.cpp:250, 815`).
- **openfx-misc plugins that still hold premult state on the node:** `premultChanged`, plus a `changedClip` auto-toggle that ticks `(Un)premult by` itself when the input clip reports premultiplied (Grade `Grade.cpp:155, 815, 1192-1217, 1408`, and about 20 more plugins). Many of those are already shadowed by native nodes (`Engine/Nodes/Color/`) or will be deleted by M145 - OFX Fork Plugin Purge; only the ones that stay OFX are touched here.
- **The Engine-side workaround for those parameters:** `hideDeprecatedPremultKnobs` hides them by name (`Engine/OfxEffectInstance.cpp:511, 645-670`, `.h:357`). Test mirrors of the parameters are in `Tests/OfxTestPlugins/natronTest/RoutingTestPlugins.cpp:574, 765`, `Tests/NativeParity.cpp:212`, `Tests/ChannelSetRender_Test.cpp:701-736` and `Engine/Nodes/README.md:955`.

This milestone removes all of that. Premult stops being state that flows through the graph: on an OFX node that reads it, the artist sets one host knob, **Input alpha: Premultiplied / Unpremultiplied** (default Premultiplied), and the host hands that value to the plugin as its input clips' premult property. The colour family's explicit `(Un)premult by` convenience stays.

## Scope

**In:**
- Strip premult storage, derivation and read-back from the OpenFX HostSupport fork, and bump the submodule.
- A host-side `Input alpha` knob, near the top of the panel, on every OFX node whose plugin reads clip premult. The host answers the input clips' premult property from it, and discards whatever the plugin sets as its output premult.
- openfx-misc fork: delete `premultChanged` and the clip-premult auto-toggle from the plugins that stay OFX after M145 - OFX Fork Plugin Purge.
- Delete the host hide list and the test-side mirrors, and add an absence guard that covers every plugin.

**Out:**
- Read and Write: already native (M75 - Native Read, M76 - Native Write). The openfx-io reader and writer premult params go with those plugins.
- The OCIO plugins: ported by M127 - Native OCIO Nodes and deleted by M145 - OFX Fork Plugin Purge.
- openfx-misc plugins shadowed by a native node or due to be ported: M145 - OFX Fork Plugin Purge deletes them.
- `unPremultBy`/`unPremultByChannel` and the host `(Un)premult by` selector (`Engine/Node.cpp:3273-3360`, `Image::unPremultiplyByChannel`, the `Premult` stage in native nodes). These stay.
- The Premult and Unpremult nodes, and Keyer/ChromaKeyer's "Premultiplied" output mode. These are operations, not tracked state.
- The premultiplied-samples convention for deep data (`Engine/DeepPixelOps.h:50`). It is maths, not a tracked attribute.
- Plugins' `clipPreferences.setOutputPremultiplication(...)` calls. The host discards the answer, so they are left in place to keep the forks close to upstream.
- An `Input alpha` knob on native nodes, and a premult field in per-frame metadata (M167 - Per-Frame Node Metadata carries none).

**Edge cases:**
- Old projects that saved `premultChanged` load without it. `Node.cpp:2423` iterates live knobs, so unknown serialized names are skipped. No legacy fixture is built.
- A plugin that reads clip premult only on some inputs (for example a mask input): the knob drives the source clips only, and mask clips answer Unpremultiplied.
- Changing `Input alpha` changes the node's hash and re-renders it, like any knob. It never changes anything downstream except through the pixels.

**Assumptions:**
- The fork line numbers come from the pinned ref (`OPENFX_MISC_REF cd08fb2c`, `tools/ci/local/fetch-assets.sh:176`). The plugin list comes from an older local clone, so P1.T2 re-greps it at the pin.
- `build.sh` resets submodules, so pin the `libs/OpenFX` fork branch before building.

## Phase 43.1: Host answers premult from an explicit knob and stores nothing

- [x] M43.P1.T1 — Remove premult storage and derivation from the OpenFX HostSupport fork
  - files: `libs/OpenFX/HostSupport/include/ofxhImageEffect.h`, `libs/OpenFX/HostSupport/src/ofxhImageEffect.cpp`, `libs/OpenFX/HostSupport/src/ofxhClip.cpp`, `libs/OpenFX` submodule pointer
  - approach: Delete `_outputPreMultiplication`, its copy at `:656`, `getOutputPreMultiplication()`, the premult branch of `setDefaultClipPreferences` (`:2647-2751`), the read-back at `:2947` and the debug print at `:2951`. In `setupClipPreferencesArgs` (`:2782`), keep the `kOfxImageEffectPropPreMultiplication` slot so that plugin setters do not throw, and never read it back. Clip premult becomes a host-supplied value, so change the `ofxhClip.cpp:250, 815` defaults to `kOfxImageUnPreMultiplied` and let the host's clip instance override the getter. Merge the fork PR, then bump the pointer.
  - verify: `git -C libs/OpenFX grep -n _outputPreMultiplication` is empty and `git grep kOfxImageOpaque HostSupport/src` is empty. The phase build plus full `ctest` is green.
  - size: M

- [x] M43.P1.T2 — List the OFX plugins that stay OFX and read clip premult
  - files: this milestone file (a "Plugins in scope" section); read-only over the openfx-misc fork at `cd08fb2c`, `Engine/Nodes/`, `PLAN/MILESTONES/M145-ofx-fork-plugin-purge.md` and the port milestones it lists
  - approach: For every plugin the shipped bundles provide, mark it shadowed (a native node registers its ID), to be ported (named by a port milestone M145 depends on), or lasting. For each lasting plugin, record whether it has `premultChanged` or the auto-toggle, and whether it reads `getPreMultiplication()` for its own maths (for example unpremultiplying around a filter). Rewrite P2.T1's file list from the result, adding further removal tasks (P2.T3 onward, placed before P2.T2's bump) of at most 5 files each if needed.
  - verify: Every plugin ID in the shipped bundles appears in the section with one of the three marks, and every lasting plugin that reads premult is assigned to a P2 task or to P1.T3's knob list.
  - size: M

- [x] M43.P1.T3 — Add the host `Input alpha` knob and answer clip premult from it
  - files: `Engine/OfxEffectInstance.cpp`, `Engine/OfxEffectInstance.h`, `Engine/OfxClipInstance.cpp`, `Engine/OfxClipInstance.h`
  - approach: On OFX nodes whose plugin P1.T2 marked as reading clip premult, create a host choice knob `inputAlpha` ("Input alpha": Premultiplied / Unpremultiplied, default Premultiplied) at the top of the main page, next to where other host knobs are inserted. `OfxClipInstance` answers `getPremult` (`OfxClipInstance.cpp:283`) and the fetched image's premult property (`:1473`) from that knob for source clips, and Unpremultiplied for mask clips and for nodes without the knob. Nothing reads the plugin's output premult. The knob is an ordinary knob: hashed, saved, undoable and scriptable.
  - verify: covered by P1.T4's gtest, plus a Python check that `inputAlpha` exists on one in-scope plugin and is absent on a native node and on an OFX plugin outside the list.
  - size: M

- [x] M43.P1.T4 — Gtest proving premult comes only from the knob and never flows downstream
  - files: `Tests/OfxTestPlugins/natronTest/RoutingTestPlugins.cpp`, `Tests/OfxTestPlugins/natronTest/NatronTestPluginIDs.h`, `Tests/PremultAbsence_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: Add a test plugin whose `getClipPreferences` calls `setOutputPremultiplication(eImagePreMultiplied)` (or `eImageOpaque`, selected by a param), and which records `getPreMultiplication()` from its source clip and from fetched images. Mark it in scope for the knob. In the gtest, chain two instances. Assert that the downstream node sees exactly its own `inputAlpha` value on the clip and on the image whatever the upstream node set, that flipping the upstream param leaves the upstream `NodeMetadata` and the downstream node's input unchanged, and that flipping `inputAlpha` changes only that node's hash.
  - verify: `ctest -R PremultAbsence` is green, and so is the rest of the suite.
  - size: M

## Phase 43.2: openfx-misc fork holds no premult state

- [x] M43.P2.T1 — Drop `premultChanged` and the clip-premult auto-toggle from the lasting plugins
  - files: fork `charlesangus/openfx-misc`: the lasting plugins P1.T2 lists (at most 5 files; P1.T2 adds P2.T3 onward for the rest)
  - approach: Delete the `kParamPremultChanged` define, descriptor, fetch and member, and the `changedClip` block that switches on `_srcClip->getPreMultiplication()` (pattern: Grade `:155, 785, 815-816, 960, 1192-1217, 1408`). Keep the `unPremultBy`/`unPremultByChannel` params and their maths. A plugin that reads `getPreMultiplication()` for its own maths keeps that read, which P1.T3's knob now drives.
  - verify: `git grep -n premultChanged <files>` is empty, no `changedClip` handler reads `getPreMultiplication()`, and the fork build is green.
  - size: M

- [x] M43.P2.T2 — Bump `OPENFX_MISC_REF` and check the unpremult maths
  - files: `tools/ci/local/fetch-assets.sh`
  - approach: Point the ref at the merge of the P2 removal tasks, and add a delta line to the openfx-misc comment block. After the phase build, render one lasting colour plugin with `(Un)premult by` on over a Constant(rgb 1, a 0.5).
  - verify: the render matches the M38.P10.T10 formula, and full `ctest` is green.
  - size: M

## Phase 43.3: Host cleanup and guards

- [x] M43.P3.T1 — Delete `hideDeprecatedPremultKnobs`
  - files: `Engine/OfxEffectInstance.cpp`, `Engine/OfxEffectInstance.h`, `Tests/ChannelSetRender_Test.cpp`
  - approach: Remove the call (`:511`), `hidePremultKnobOnHolder` and `hideDeprecatedPremultKnobs` (`:645-670`, `.h:357`). Rewrite `ChannelSetRender_Test.cpp:701-736` so it asserts that `getKnobByName("premultChanged")` is null on a lasting plugin, instead of asserting that the knob is secret.
  - verify: `ctest -R ChannelSetRender` is green.
  - size: M

- [x] M43.P3.T2 — Remove `premultChanged` from the test plugins, parity filter and docs
  - files: `Tests/OfxTestPlugins/natronTest/RoutingTestPlugins.cpp`, `Tests/NativeParity.cpp`, `Tests/NativeParity.h`, `Engine/Nodes/README.md`
  - approach: Delete `kParamPremultChanged` and its descriptor (`RoutingTestPlugins.cpp:574, 765`). Drop `"premultChanged"` from `isOfxInternalKnobName` (`NativeParity.cpp:212`, doc `.h:335`). Reword `README.md:955`, and document the `Input alpha` knob there.
  - verify: `ctest -R 'Native|Routing|Parity'` is green, and `git grep -n premultChanged Engine Tests` is empty.
  - size: M

- [x] M43.P3.T3 — Gtest guarding premult absence across every registered plugin
  - files: `Tests/PremultAbsence_Test.cpp`
  - approach: Iterate the app's plugin list, create each OFX and native node, and assert that no knob is named `premult`, `premultChannel`, `premultChanged`, `filePremult`, `outputPremult` or `inputPremult`. `inputAlpha` is allowed only on OFX nodes. Plugins that M145 - OFX Fork Plugin Purge will delete are exempt by ID, from P1.T2's list, until it lands. If the ctest environment does not load the fetched bundles, skip that part with a logged count and record the gap.
  - verify: `ctest -R PremultAbsence` is green, and the logged plugin count is above 100.
  - size: M

## Decisions

- 2026-10-24 — **Phases 2–3 verified (PM):** fast build on a2c3d52e7 is 390/390 on both ctest legs. `PremultAbsence` logs 211 plugins checked, 41 exempt. The six openfx-io OCIO plug-ins (`premult`, `premultChannel`) joined the exemption list under M127 - Native OCIO Nodes, since M145 deletes them. P2.T2's unpremult maths is covered by `ChannelSetRenderUnPremultByTest.GainIsUnPremultipliedByTheSelectedChannel` (a lasting colour plugin with `(Un)premult by` against the M38 formula), which passes on both legs.
- 2026-10-24 — **Phases 2–3 scope adjustments (PM).** (1) `CImg/CImgOperator.{cpp,h}` (CImgBilateralGuided) read the `_premultChanged` member that P2.T1 removes and had the same auto-toggle, so they join P2.T1's files. (2) NoOp stays OFX but still had an `outputPremult` choice the Phase 1 audit missed. That is premult state held by a node, which the user's ruling rules out, and the host now discards it, so the fork removes the param and its `setOutputPremultiplication` call rather than exempting NoOp from the guard. (3) The guard's exemption list (plugins M145 deletes) is M122's ColorMatrix, ColorSuppress, HSVTool, HueCorrect, HueCorrect1, HueKeyer, Premult, Unpremult and the 26 ColorTransform IDs, plus M130's GodRays. The test fails once an exempt ID is no longer loaded, so each entry is removed as M145 lands. (4) The gate grep can't be empty over `Tests`, because the guard tests must name the banned knobs. It becomes `git grep -n -E 'premultChanged|filePremult|outputPremult|inputPremult|hideDeprecatedPremult|_outputPreMultiplication' Engine Gui libs/OpenFX/HostSupport` empty, plus the same over `Tests` matching only `PremultAbsence_Test.cpp` and `ChannelSetRender_Test.cpp`. `inputAlpha` is checked by the guard test, not the grep, because of the unrelated Deep `targetInputAlpha` knob.

- 2026-10-24 — **User decision: no `Input alpha` knob.** P1.T2's audit found no plugin that stays OFX and reads clip premult for its maths (only `premultChanged`/auto-toggle UI in CImgFilter, ClipTest and DenoiseSharpen), so the knob would ship on no plugin. Clips and fetched images answer Unpremultiplied; premult storage, derivation, read-back and the plugin params are still removed. P1.T3 becomes: clip premult answers Unpremultiplied (no knob). P1.T4's test proves a plugin's reported output premult is discarded and never reaches downstream clips, images or `NodeMetadata`. P3.T3's guard bans `inputAlpha` too. P2.T1's files: `CImg/CImgFilter.cpp`, `CImg/CImgFilter.h`, `ClipTest/ClipTest.cpp`, `DenoiseSharpen/DenoiseSharpen.cpp`; no P2.T3+.

- 2026-10-24 — **M43 revived, scoped to what 38.2 left:** The user's ruling is that nodes hold no premult information, and they chose to revive M43 rather than file a new milestone. On `origin/main` the remaining state is in the HostSupport fork and in plugin params that the host only hides. The Engine enum and metadata the inbox report cited were already deleted by `30c1f0ceb`.
- 2026-10-24 — **OFX premult comes from an explicit per-node host knob, `Input alpha`, default Premultiplied:** The user found a fixed constant odd, since grade-like plugins want unpremultiplied input and filters want premultiplied, and asked for a host knob near the top of the node. It is set by the artist, never inferred and never passed downstream, so premult is no longer tracked state. Only OFX plugins that read clip premult get it. Premultiplied is the default because that is how image data normally arrives.
- 2026-10-24 — **Only plugins that stay OFX are edited:** Read and Write are already native, and the OCIO plugins and shadowed or to-be-ported misc plugins are deleted by M145 - OFX Fork Plugin Purge, so editing them would be wasted fork churn (user, grill).
- 2026-10-24 — **`unPremultBy`/`unPremultByChannel` and the host `(Un)premult by` selector are kept:** They are an explicit per-node convenience, not tracked state (M38, 2026-09-21 decision).

**Verification gate:** All three phase builds (`tools/ci/remote/build.sh fast --tests`) and full `ctest` are green, including `PremultAbsence` and `ChannelSetRender`. `git grep -n -E 'premultChanged|filePremult|outputPremult|inputPremult|hideDeprecatedPremult|_outputPreMultiplication'` over `Engine Gui Tests libs/OpenFX/HostSupport` is empty. The `(Un)premult by` render matches M38.P10.T10, and a Python check shows `inputAlpha` on an in-scope OFX plugin and on no native node.
