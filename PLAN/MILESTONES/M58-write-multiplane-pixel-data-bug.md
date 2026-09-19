# Milestone 58: Write's "All Layers" output copies one layer's pixel data into every layer

Discovered while diagnosing M57.P1.T2, then bisected out of that milestone because it predates
M39 (confirmed pre-existing on `3e14c2a1e`). The user-visible symptom: with a Write node's
"All Layers" checkbox (`processAllLayers` OFX param) checked, the rendered EXR does not contain
every input layer.

**Root cause (consultant investigation, 2026-09-19 — see `## Decisions`):** the OFX params of a
bundled writer are knobs of the *container* (`Write1`, `Engine/OfxParamInstance.cpp:315-329`),
so toggling `processAllLayers` bumps only the container's `knobsAge`. `WriteNode` is a
`NodeGroup`, so the embedded encoder (`internalEncoderNode`) is a real render-graph node with
its own hash, which `Node::computeHashRecursive` (`Engine/Node.cpp:884-923`) never revisits.
Its per-hash `ActionsCache` entry for `getComponentsNeededAndProduced` therefore keeps serving
the plane set computed for the previous render: after a render with the box off, checking it
and rendering again writes only `R,G,B,A` — the extra layers take the pass-through branch in
`EffectInstanceRenderRoI.cpp:492-510` and are never encoded. `OutputSchedulerThread.cpp:2272`
(`isWriteNode ? isWriteNode->getHash() : …`) is an existing partial workaround that makes the
scheduler's *request* fresh while `renderRoI`'s lookups stay stale — which is why "checked
before the first render" and ON→OFF look correct by accident.

Repro kit (scratch, not committed): `build/m58-repro/` — `run.sh <release|debug> [M58_TOGGLE=1]
[M58_RECONNECT=1]`, `run-gui.sh` (Xvfb), `trace2.gdb`, `README.txt`. Trustworthy pixel checks
are `oiiotool --stats -a` or `oiiotool f.exr --subimage N --printstats`; `--printstats -a`
mis-reports alpha on later parts, and Python `read_image()` without `seek_subimage()` returns
part 0 every time.

## Phase 58.1: Write container knob changes must reach the embedded encoder's hash

- [x] M58.P1.T1 — Add a committed three-layer flat EXR fixture and its generator
  - files: Tests/fixtures/make-flat-layers-fixture.py (new), Tests/fixtures/flat-three-layers.exr (new, ~1 KB)
  - approach: mirror Tests/fixtures/make-deep-fixtures.py (Python OpenImageIO, run via tools/ci/local/devshell.sh): 8x8 single-part half EXR with channels R,G,B,A=(1,0,0,1), diffuse.R/G/B=(0,1,0), specular.R/G/B=(0,0,1); document the values in the script header exactly as make-deep-fixtures.py does. Read by ReadOIIO only, so half/zip is fine (the output side is what Tests/FlatExrReader.h constrains).
  - verify: `oiiotool --stats Tests/fixtures/flat-three-layers.exr` prints channel list R,G,B,A,diffuse.R,diffuse.G,diffuse.B,specular.R,specular.G,specular.B and Stats Avg 1 0 0 1 0 1 0 0 0 1; regenerating with the script is byte-identical.
  - size: S
- [ ] M58.P1.T2 — Add a gtest that toggles All Layers between two renders and checks every layer's pixels
  - files: Tests/WriteAllLayers_Test.cpp (new), Tests/CMakeLists.txt
  - approach: TEST_F(BaseTest, WriteAllLayersToggleAfterRenderWritesEveryLayer): create ReadOIIO via CreateNodeArgs + addParamDefaultValue(kOfxImageEffectFileParamName, NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr") (pattern: Tests/Metadata_Test.cpp:608); create the writer with createNode(_writeOIIOPluginID) — AppInstance.cpp:1142 wraps it in the WriteNode container, which is the buggy path; set knobs by name on the container: partSplitting="single", bitDepth="32f", compression="none" (FlatExrReader.h layout); render frame 1 via startWritersRendering (pattern: Tests/RenderRange_Test.cpp:130-140) into a QTemporaryDir with processAllLayers=false -> assert readFlatExr channels == {R,G,B,A}; set processAllLayers=true, new filename, render -> assert channels include diffuse.R/G/B and specular.R/G/B and pixel (x1,y1) values are diffuse=(0,1,0), specular=(0,0,1), R,G,B,A=(1,0,0,1); set false again, render -> RGBA only. Also a second TEST_F where the box is checked before the first render (must already pass; guards the accidental-correctness path). Register the .cpp in Tests/CMakeLists.txt.
  - verify: on the current (unfixed) tree `tools/ci/local/test.sh ctest debug` shows the new toggle test FAILING at the post-toggle channel assertion (only R,G,B,A) and the checked-first test passing; ctest count goes 216 -> 218.
  - size: M
- [ ] M58.P1.T3 — Fold the Read/Write container's knob age into the embedded node's hash and recompute it with the container
  - files: Engine/Node.cpp (computeHashInternal ~:807, computeHashRecursive ~:884), Engine/OutputSchedulerThread.cpp (:2272 workaround removal)
  - approach: append `ioContainer->getKnobsAge()` to an embedded node's hash in `computeHashInternal` (after `_imp->hash.append(_imp->knobsAge)`); in `computeHashRecursive`, after a WriteNode/ReadNode container's own hash changed, recurse into `getEmbeddedWriter()`/`getEmbeddedReader()` (precedent: the disabled-group inner-node hash change at Node.cpp:5375-5391); then replace the OutputSchedulerThread.cpp:2272 ternary with `activeInputToRender->getHash()` so scheduler, request pass and renderRoI share one hash. No openfx-io change, no NATRON_CACHE_VERSION bump (the hash formula change only orphans in-memory entries for embedded encoder outputs).
  - verify: `build/m58-repro/run.sh release M58_TOGGLE=1` prints three channel lists (diffuse, specular, RGBA) with `oiiotool --stats -a` averages 0 1 0 / 0 0 1 / 1 0 0 1; gdb `trace2.gdb` shows `onNodeHashChanged node=internalEncoderNode` immediately after the toggle and an `OFX getClipComponents ACTION` before `getImagePlane`; T2's tests pass; `tools/ci/local/test.sh ctest debug` and `test.sh smoke debug` green; `build/m58-repro/run-gui.sh M58_VIEW=read M58_TOGGLE=1` (Xvfb GUI, in-process render like the user) prints three channel lists.
  - size: M

**Verification gate:** all 218 ctest cases and the smoke test green on debug; `run.sh release M58_TOGGLE=1` and `run-gui.sh M58_VIEW=read M58_TOGGLE=1` both emit diffuse/specular/RGBA parts with the expected solid colours (checked with `oiiotool --stats -a`, not `--printstats`).

## Decisions

- 2026-09-19 — **Reframed from "identical pixel data in every layer" to "stale embedded-encoder
  hash after toggling All Layers"**: the stub's symptom did not reproduce on release or debug
  binaries, headless or GUI, across ~a dozen input layouts; gdb shows three distinct source
  buffers per plane and `OfxClipInstance` routes each plane string to its own `Natron::Image`.
  The earlier "diffuse copied into all parts" finding matches exactly what a Python-OIIO
  `read_image()` loop without `seek_subimage()` prints (part 0 every time), so it is judged a
  verification artefact. The user's original M57 symptom ("checking the box does not write every
  input layer") reproduces deterministically when the box is toggled after a first render, and is
  what this milestone fixes. Milestone name/ID kept (stable IDs).
- 2026-09-19 — **Fix is host-side, hash-derivation form**: fold the container's `knobsAge` into
  the embedded node's hash and recurse hash recomputation from container to embedded node,
  rather than bumping the embedded node's age from `incrementKnobsAge()` (which would miss
  `setKnobsAge` and `incrementKnobsAge_internal` callers). The `OutputSchedulerThread.cpp:2272`
  workaround becomes redundant and is removed for a single source of truth.
- 2026-09-19 — Out of scope, noted for a follow-up: `EffectInstance::getAvailableLayers`
  (`EffectInstance.cpp:4476`) queries the *input's* components-needed cache with *this* node's
  render hash, polluting the input's `ActionsCache` with foreign hash entries.
