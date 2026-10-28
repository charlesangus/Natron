# M140 - Native Metadata Utilities

Port the three OFX example metadata nodes that M77 - Native Metadata Nodes left without a native equivalent (`org.openfx.examples.metadataCompare`, `metadataContribute`, `metadataTimeCode`) to native nodes in `Engine/Nodes/Metadata/`, then retire the OFX versions. Retiring them drops the whole `openfx-metadata` build from `tools/ci/local/fetch-assets.sh`, which also removes the `metadataPrint` test fixture. `Tests/Metadata_Test.cpp` and `Tests/OfxMetadataBridge_Test.cpp` need those OFX plug-ins as fixtures for `OfxMetadataBridge` and the OpenFX metadata suite. So this milestone adds an in-repo, test-only OFX plug-in bundle (Contribute, Print and TimeCode equivalents) that CMake builds and the test binary puts on its OFX plug-in path.

Deps: M77 - Native Metadata Nodes, M120 - Native Node Port Kit.

M120 - Native Node Port Kit is not built yet. Every native-node task below is written against its planned scheme: a per-node `Engine/Nodes/Metadata/<Node>.reg.inc` registration fragment, a per-node `Engine/Nodes/Metadata/docs/<Node>.md`, a per-node `Tests/Native/Native<Node>_Test.cpp` that asserts the node's own ID, major and label through M120's `expectNativeIdentity` helper, one `Engine/OfxRetired/<ofx-id>` marker per retired ID, and M120's test-only OFX bundle scaffolding (`Tests/OfxTestPlugins/`, `natron_add_test_ofx_bundle`, and the `OFX_PLUGIN_PATH` wiring in `Tests/wmain.cpp`). At promotion (PLAN-FORMAT §5a), re-check these names against what M120 actually built and rewrite the briefs that differ. If M120 has not landed, the native-node tasks fall back to M77's way: an include plus a `registerBuiltInPlugin<T>` line in `AppManager::loadBuiltinNodePlugins()`.

## Scope

**In:**
- Three native nodes: CompareMetadata, ContributeMetadata and TimeCodeMetadata. Each derives from `NativeEffectBase`, shares the Metadata menu group (`PLUGIN_GROUP_METADATA`) with the M77 nodes, and has help text, a docs file and a gtest.
- A shared metadata-to-text helper, extracted from `ViewMetadata.cpp`, which CompareMetadata and ViewMetadata both use.
- A test-only OFX bundle built in-tree from `Tests/`, written against the raw OpenFX C API (`ofxImageEffect.h`, `ofxProperty.h`, `ofxMetadata.h`). It holds three plug-ins that replace the fixtures Contribute, Print and TimeCode.
- `Tests/Metadata_Test.cpp` and `Tests/OfxMetadataBridge_Test.cpp` repointed at those plug-ins, with the bridge coverage kept.
- Retirement of the three OFX IDs: one `Engine/OfxRetired/<ofx-id>` marker each, and the `OfxEffectInstance.cpp` grouping special case, the `NativePluginList_Test.cpp` "OFX metadata nodes grouped" case and the `smoke_test.py` bundle checks removed.
- Removal of the `openfx-metadata` build (pin, clone, make, copy, verify), the `build/assets/TestPlugins/` directory and everything that reads it, from `fetch-assets.sh`, `test.sh`, the local CI README and the four workflow cache paths.

**Out:**
- Behaviour changes to the ported nodes beyond what the native model forces. They are faithful ports of the OFX examples, not redesigns. A configurable key table is not wanted (M77's decision: ModifyMetadata's slots already do that).
- Porting `metadataCopy`, `metadataModify`, `metadataPrint` and `metadataView`: M77 - Native Metadata Nodes already replaced the first, second and fourth. Print is replaced only as a test fixture, not as a node.
- Dropping or editing anything in `libs/OpenFX` or the openfx-misc fork pin.
- Renaming the native IDs to plain IDs: M111 - Native Node Identity Cleanup. The new nodes follow M77's `fr.natron.<Name>Metadata` convention and M111 renames them with the rest.
- Making `Tests/Metadata_Test.cpp`'s other fixtures (ColorWheel, ColorMatrix) independent of OFX plug-ins that other milestones retire. M120 - Native Node Port Kit (P3.T2) owns that file's swap. This milestone only changes the three metadata IDs there, applied on top of M120's version.
- A GUI preview mode for CompareMetadata beyond the read-only text display.

**Edge cases:**
- CompareMetadata with B disconnected: B's metadata is empty, so every A key reads as "A only"; it does not error. It passes A's pixels through either way.
- CompareMetadata with identical metadata: the display is empty. The display is read-only, not persistent, and refreshing it does not mark the project dirty or trigger a render (the M77 ViewMetadata requirement).
- Compare value text: vectors read comma-separated, as in the OFX example, and `ofx/viewnames`-style string vectors that `ImageMetadata` cannot hold are not compared (the bridge already skips them).
- ContributeMetadata "drop one key" with a key that upstream does not carry: no-op. "Inherit nothing" starts from an empty set, so upstream keys are gone but the node's own keys (and the `ofx/framerate` of 30) are present.
- TimeCode counts: a negative offset (frames before the origin) wraps back through 00:00:00:00 into the previous day, and a count past 23:59:59:FF wraps to 00:00:00:00 (as the OFX example does). A rate below 1 clamps to a counted rate of 1. A start-timecode string with fewer than four numbers reads as 00:00:00:00.
- TimeCode "rate from metadata" with upstream carrying no `ofx/framerate`: falls back to the `rate` knob.
- A project saved with an `org.openfx.examples.metadata*` node loads the missing-plug-in node, the same as any absent plug-in (clean break, no importer).
- A stale `build/assets/Plugins/metadata*.ofx.bundle` or `build/assets/TestPlugins/` left from an earlier `fetch-assets.sh` run must not make the test suite pass or the smoke test fail by accident. The new fetch step removes them, and the retired-ID list hides the three IDs from the host even if the bundles are still present.
- Test plug-in on the OFX path: the test bundle must be on the path for the test binary and gtest discovery, but not in `build/assets/Plugins/` (which `smoke_test.py` asserts is the exact shipped set and `stage-bundle.sh` ships).

**Assumptions:**
- Unconfirmed: the openfx-metadata fork's `ofxsMetadata.h` C++ wrapper is not in the in-repo `libs/OpenFX/Support/`. Scouted: only `libs/OpenFX/include/ofxMetadata.h` is, so the test plug-in uses the raw C suites rather than the Support library.
- Unconfirmed: the native ContributeMetadata keeps the OFX example's six demo keys, under the prefix `fr.natron.ContributeMetadata.`, plus `ofx/framerate` = 30. The demo-key shape is exactly what the bridge tests relied on, and the namespace follows the node ID.
- M120 - Native Node Port Kit lands before this milestone is claimed (the dep). If it does not, the fallback is stated in the intro.
- Linux-x86-64 is the only bundle layout the test plug-in must build for locally. Use `${CMAKE_SYSTEM_NAME}`-keyed arch directory naming so CI on other platforms does not break the configure.

## Phase 140.1: Native nodes

All three derive from `NativeEffectBase`, modelled on M77's `Engine/Nodes/Metadata/CopyMetadata.h/.cpp` and `ViewMetadata.h/.cpp`: one image input (two for Compare), `isIdentity()` returns input 0, no `render()`, `deriveOutputMetadata()` over `ImageMetadata` (`Engine/Nodes/Metadata/ImageMetadata.h`) using `getUpstreamMetadata()` / `getInputMetadata(n, ...)`. Grouping `PLUGIN_GROUP_METADATA` in each `getNativePluginDescription()`. Registration, docs, identity assertion and test follow M120's per-node scheme (see the intro). IDs `PLUGINID_NATRON_COMPAREMETADATA` / `CONTRIBUTEMETADATA` / `TIMECODEMETADATA` = `fr.natron.<Name>`. Metadata nodes have no per-node icon: the `.reg.inc` icon is empty, as in M77, because the Metadata category icon covers them.

- [ ] M140.P1.T1 — Shared metadata text formatting
  - files: new `Engine/Nodes/Metadata/MetadataText.h/.cpp`, `Engine/Nodes/Metadata/ViewMetadata.cpp`, new `Tests/Native/NativeMetadataText_Test.cpp`
  - approach: move `escapeHtml()`, `joinValues()` and `formatValue()` (now in `ViewMetadata.cpp`'s anonymous namespace, `:46-130`) into a `MetadataText` namespace with the exact same output, and have `ViewMetadata` call it. Add a plain-text counterpart that CompareMetadata needs: `formatValuePlain(const ImageMetadata::Value&)` returning the comma-separated value with no HTML. Pure refactor for the HTML path: `ViewMetadata`'s displayed text must not change byte for byte.
  - verify: at the phase build, a gtest covers each `ImageMetadataType` (int, double, string, int vector, double vector) through the HTML and plain formatters, plus HTML escaping; `NativeViewMetadata_Test` passes unchanged.
  - size: M

- [ ] M140.P1.T2 — CompareMetadata node
  - files: new `Engine/Nodes/Metadata/CompareMetadata.h/.cpp`, `Engine/Nodes/Metadata/CompareMetadata.reg.inc` (M120 scheme), `Engine/Nodes/Metadata/docs/CompareMetadata.md`, new `Tests/Native/NativeCompareMetadata_Test.cpp`
  - approach: inputs A (pixels, identity) and optional B, matching CopyMetadata's input setup (`CopyMetadata.cpp`). A passes through untouched and its metadata is the output metadata. The `differences` display is a multi-line read-only `KnobString` built and refreshed like ViewMetadata's (not animated, not persistent, not evaluate-on-change; Refresh button, time change, upstream metadata change; a time-driven refresh must not dirty the project or trigger a render). Compare the output metadata of A and B at the current frame by walking both ordered `ImageMetadata` maps once (merge-walk, ascending key). Lines: `A only: <key>=<value>`, `B only: <key>=<value>`, `differs: <key>: A=<value> B=<value>`; equal keys print nothing. Use `MetadataText::formatValuePlain` from P1.T1 and keep the display plain text (no HTML table). Hide the unused Channels selector as M77 did. Description string: one paragraph.
  - verify: gtest with two `MetadataSource` test nodes (`Tests/NativeMetadataTestEffect.h`) with different tags and `setExtraMetadata()` keys: the text has an A-only, a B-only and a differs line in key order; identical sources give empty text; B disconnected gives all A-only lines; the text follows a frame change and a knob edit on a source. Identity test through `expectNativeIdentity` asserts ID, major and label; grouping is `Metadata`.
  - size: M

- [ ] M140.P1.T3 — ContributeMetadata node
  - files: new `Engine/Nodes/Metadata/ContributeMetadata.h/.cpp`, `Engine/Nodes/Metadata/ContributeMetadata.reg.inc`, `Engine/Nodes/Metadata/docs/ContributeMetadata.md`, new `Tests/Native/NativeContributeMetadata_Test.cpp`
  - approach: port of `metadataContribute` (see `build/assets/plugin-src/openfx-metadata/Support/Plugins/MetadataContribute/metadataContribute.cpp`, produced by `fetch-assets.sh`; read it before it is removed in P4). Knobs `note` (single-line `KnobString`), `mode` (choice: inherit all / drop one key / inherit nothing), `dropKey` (`KnobString`, default `ofx/sampletype`). `deriveOutputMetadata()` starts from upstream (inherit all), upstream minus `dropKey` (drop one key), or empty (inherit nothing), then sets six demo keys under the prefix `fr.natron.ContributeMetadata.`: `note` (string), `revision` (int 1), `quality` (double 0.75), `tags` (string vector is not representable, so publish the two tags as one comma-joined string), `renderRegion` (int vector 0,0,1280,720), `weights` (double vector 1,0.5,0.25); and `ofx/framerate` = 30.0. The `tags` change is a forced deviation: `ImageMetadata` has no string vector type (the bridge skips them). Record it in the node's docs file.
  - verify: gtest on `MetadataSource` → ContributeMetadata: every key and type present; `note` follows the knob (and a keyframed or expression-driven value is not required: the knob is not animated); each mode (inherit-all keeps upstream keys, drop-one removes exactly `dropKey` and keeps the rest, inherit-nothing leaves only the node's own keys); `ofx/framerate` is 30 even when upstream says otherwise; metadata is recomputed after a knob edit. Identity test via `expectNativeIdentity`.
  - size: M

- [ ] M140.P1.T4 — TimeCodeMetadata node
  - files: new `Engine/Nodes/Metadata/TimeCodeMetadata.h/.cpp`, `Engine/Nodes/Metadata/TimeCodeMetadata.reg.inc`, `Engine/Nodes/Metadata/docs/TimeCodeMetadata.md`, new `Tests/Native/NativeTimeCodeMetadata_Test.cpp`
  - approach: port of `metadataTimeCode` (see `.../MetadataTimeCode/metadataTimeCode.cpp`; read it before it is removed in P4). Knobs `startTimecode` (string, default `01:00:00:00`), `rate` (double, default 24, range 1-1000), `rateFromMetadata` (bool, default on), `startFrame` (int, default 1), `useStartFrame` (bool, default off). Implement the helper functions as pure static functions: `parseTimecode`, `timecodeToFrames`, `framesToTimecode`, `roundedRate`, with the same arithmetic as the example (non-drop, 24-hour wrap, `floor(time - origin + 0.5)` offset). `deriveOutputMetadata(time)` publishes `ofx/timecode` (string) and `ofx/framerate` (double). When `rateFromMetadata` is on it reads `ofx/framerate` from `getUpstreamMetadata()` and falls back to `rate`. The origin is frame 1 unless `useStartFrame`.
  - verify: gtest on the pure helpers (parse with separators and short input, wrap past 24 hours, negative offsets); a gtest with a `MetadataSource` upstream: timecode advances one frame per timeline frame, the frames field rolls into seconds at counted rate + 1, the rate comes from upstream when on and from the knob when off or absent, `useStartFrame` moves the origin. Identity test via `expectNativeIdentity`.
  - size: M

**Phase gate:** at the phase build (one build for all of P1: batch these four tasks), the new gtests pass; `ctest -R "NativeCompareMetadata|NativeContributeMetadata|NativeTimeCodeMetadata|NativeMetadataText|NativeViewMetadata"` is green; the Metadata menu shows seven native nodes plus the OFX ones that P4 has not retired yet.

## Phase 140.2: In-repo test-only OFX plug-in

One OFX bundle, built from `Tests/`, holding three plug-ins. The bundle is built against `libs/OpenFX/include` only, with the raw C API (the Support library has no metadata wrapper, see Assumptions). Plug-in IDs `fr.natron.test.metadataContribute`, `fr.natron.test.metadataPrint`, `fr.natron.test.metadataTimeCode`, so the old IDs cannot be confused with them. Each is a filter-context effect with one source clip and an output clip that copies pixels through (RGBA/Alpha, UByte/UShort/Float), as the examples do. Behaviour matches the example it replaces, because the existing tests assert it.

- [ ] M140.P2.T1 — Metadata test bundle and the Print plug-in
  - files: new `Tests/OfxTestPlugins/natronTestMetadata/CMakeLists.txt`, new `Tests/OfxTestPlugins/natronTestMetadata/MetadataBridgeTestPlugins.cpp` (entry points, shared helpers, Print)
  - approach:
    - Add a `natronTestMetadata` bundle with M120 - Native Node Port Kit's `natron_add_test_ofx_bundle`, in its own directory, so M120's scaffolding and `natronTest` bundle stay untouched. It is a separate module because it uses the raw C API and exports its own `OfxGetNumberOfPlugins`, `OfxGetPlugin` and `OfxSetHost`. M120's `Tests/wmain.cpp` wiring already puts the test-plugin directory on `OFX_PLUGIN_PATH`.
    - Shared helpers in the .cpp: fetch the suites (`OfxImageEffectSuiteV1`, `OfxPropertySuiteV1`, `OfxParameterSuiteV1`, the metadata suite from `ofxMetadata.h`), define the source and output clips, and a pixel-copy render (RGBA/Alpha, UByte/UShort/Float/Half, as in the example's `copyPixels`).
    - Print: `render` reads its source clip's metadata at the render time through the clip-metadata suite and writes each key to the host log (the example's `logMetadata`), then copies pixels. It exists so a render-time reader runs concurrently with metadata changes.
  - verify: at the phase build, the bundle exists at the path above, `OFX_PLUGIN_PATH=build/assets/Plugins ./Tests --gtest_list_tests` still lists the suite (discovery works), and a short gtest added to M120 - Native Node Port Kit's `Tests/OfxTestPlugin_Test.cpp` shows `fr.natron.test.metadataPrint` is in `getPluginIDs()` and creates a node. Also confirm `build/assets/Plugins/` and `tools/release/stage-bundle.sh` input do not contain the bundle.
  - size: M

- [ ] M140.P2.T2 — Contribute plug-in
  - files: `Tests/OfxTestPlugins/natronTestMetadata/MetadataBridgeTestPlugins.cpp`, `Tests/OfxTestPlugin_Test.cpp`
  - approach: add `fr.natron.test.metadataContribute` with the example's params: `note` (single-line string, not animated), `mode` (choice: inherit all / drop one key / inherit nothing) and `dropKey` (string, default `ofx/sampletype`). In the get-metadata action, set the six demo keys through the clip-metadata suite's typed setters, one per suite entry point (string, int, double, string-N, int-N, double-N) under the prefix `org.openfx.examples.metadataContribute.` (keep this exact prefix: `Tests/Metadata_Test.cpp`'s `kContributedKeys` and the bridge tests key on it, and it avoids any clash with the native node's `fr.natron.ContributeMetadata.` keys), and `ofx/framerate` = 30. Apply the inheritance mode through the inheritance property set (retained keys, source-clip list) as the example does. Source: `build/assets/plugin-src/openfx-metadata/Support/Plugins/MetadataContribute/metadataContribute.cpp`.
  - verify: at the phase build, a gtest creates the plug-in, reads its output clip's metadata through `OfxClipInstance::getCurrentMetadata` and finds all seven keys with the right types and dimensions; `mode` drop-one and inherit-nothing change what a downstream native `ViewMetadata`-style read sees.
  - size: M

- [ ] M140.P2.T3 — TimeCode plug-in
  - files: `Tests/OfxTestPlugins/natronTestMetadata/MetadataBridgeTestPlugins.cpp`, `Tests/OfxTestPlugin_Test.cpp`
  - approach: add `fr.natron.test.metadataTimeCode` with the example's five params (`startTimecode`, `rate`, `rateFromMetadata`, `startFrame`, `useStartFrame`; same names and defaults, because `Tests/Metadata_Test.cpp:995-` looks them up by name). In get-metadata it computes the timecode and writes `ofx/timecode` and `ofx/framerate`, reading the rate from the source clip's `ofx/framerate` when `rateFromMetadata` is on. The same arithmetic as P1.T4's helpers (non-drop, 24-hour wrap, `floor(time - origin + 0.5)`), written out again here in a few lines, since test code must not link to a native node. Source: `.../MetadataTimeCode/metadataTimeCode.cpp`.
  - verify: at the phase build, the gtest reads the output clip metadata at frames around the one-second rollover and gets the expected `ofx/timecode` and `ofx/framerate`.
  - size: M

**Phase gate:** at the phase build (one build for P2: batch these three tasks), `ctest -R OfxTestPlugin` passes and the bundle is present only under the build tree.

## Phase 140.3: Repoint the bridge tests

- [ ] M140.P3.T1 — Metadata_Test and OfxMetadataBridge_Test fixtures
  - files: `Tests/Metadata_Test.cpp`, `Tests/OfxMetadataBridge_Test.cpp`
  - approach: apply on top of M120 - Native Node Port Kit's P3.T2 version of `Metadata_Test.cpp`. Replace `kMetadataContributeID`, `kMetadataPrintID` and `kMetadataTimeCodeID` (`Metadata_Test.cpp:69-71`) with the `fr.natron.test.metadata*` IDs; leave `kContributeNoteKey` and `kContributedKeys` with the `org.openfx.examples.metadataContribute.` prefix, which the test plug-in keeps. Change `kMetadataContributeID` (`OfxMetadataBridge_Test.cpp:75`) the same way, and change the header comments that say the plug-ins ship with the product. No assertion changes: the point is that the same assertions pass against the new fixture. `OfxMetadataBridge_Test.cpp`'s `net.sf.openfx.ColorMatrixPlugin` fixture (`:76-77`) is M120 - Native Node Port Kit's to swap (its P3.T2), so leave it.
  - verify: at the phase build, `ctest -R "MetadataPluginFixture|OfxMetadataBridge"` passes with the same case count as before. `grep -n "org.openfx.examples.metadata\(Contribute\|Print\|TimeCode\|Compare\)\"" Tests/*.cpp` finds no node ID, only the key prefix in `kContributeNoteKey`/`kContributedKeys`.
  - size: M

**Phase gate:** the full `Metadata_Test`/`OfxMetadataBridge_Test` suites pass against the in-repo bundle with `build/assets/Plugins/` still holding the old OFX bundles (so a pass proves nothing depends on them, check by also running with those bundles removed from `OFX_PLUGIN_PATH`).

## Phase 140.4: Retire the OFX metadata nodes and the asset fetch

Do this phase last: it removes the fixtures' old source. Native nodes are in from P1; the test bundle from P2; the tests from P3.

- [ ] M140.P4.T1 — Retired-ID list and host special cases
  - files: new `Engine/OfxRetired/org.openfx.examples.metadataCompare`, `…metadataContribute` and `…metadataTimeCode` markers, `Engine/OfxEffectInstance.cpp`, `Tests/Native/NativePluginList_Test.cpp`
  - approach: per M120 - Native Node Port Kit's scheme, add one marker each for `org.openfx.examples.metadataCompare`, `org.openfx.examples.metadataContribute` and `org.openfx.examples.metadataTimeCode`, each naming its native replacement. Remove the `stdIdentifier == "org.openfx.examples.metadata..."` branch in `OfxEffectInstance.cpp` (`:1500`), which was only there so these OFX nodes joined the Metadata menu folder. Replace `NativePluginListTest.OfxMetadataNodesAreGroupedWithTheNativeOnes` (`NativePluginList_Test.cpp:~178-195`) with a test that the three IDs are absent from `getPluginIDs()`; the new native nodes' grouping is covered by their own tests.
  - verify: at the phase build, the replacement test passes even with the stale old bundles still on `OFX_PLUGIN_PATH` (the retired markers hide them) and the full ctest is green. M120's `OfxRetired_Test` still passes.
  - size: M

- [ ] M140.P4.T2 — Remove the openfx-metadata build from fetch-assets
  - files: `tools/ci/local/fetch-assets.sh`, `tools/ci/local/README.md`
  - approach: delete `OPENFX_METADATA_REPO/REF`, `TEST_PLUGINS_TARGET`, the `openfx-metadata=` entry in `PLUGINS_WANT`, `METADATA_PLUGIN_DIRS/NAMES/TARGETS`, `METADATA_OFX`, `metadata_plugins_present()` and its use in the stamp check, the `clone_at_ref` of `openfx-metadata`, the whole "openfx-metadata plugins" build block (`make -C .../Support/Plugins/<dir>`), the `cp -R` into `Plugins/` and `TestPlugins/`, and the entries in the post-build verify loops and the verify_plugin_loads probe expectations. Because `PLUGINS_WANT` changes, the stamp mismatches and a stale tree rebuilds: also delete any leftover `Plugins/metadata*.ofx.bundle`, `TestPlugins/` and `plugin-src/openfx-metadata/` by literal path, below the assets dir. Rewrite the README paragraph at `:78-85` to say the metadata test bundle is built by CMake from `Tests/OfxTestPlugins/`.
  - verify: `bash -n tools/ci/local/fetch-assets.sh`; `grep -n "metadata\|TestPlugins" tools/ci/local/fetch-assets.sh` finds no openfx-metadata reference; at the phase build, a fetch-assets run from a build/assets that holds the old bundles ends with `Plugins/` containing only Arena, CImg, IO and Misc and no `TestPlugins/`.
  - size: M

- [ ] M140.P4.T3 — Test runner and smoke test
  - files: `tools/ci/local/test.sh`, `tools/ci/smoke_test.py`
  - approach: in `test.sh` remove `TEST_PLUGINS_DIR`, the non-empty `TestPlugins` check and the `OFX_PLUGIN_PATH` append for `ctest` (`:184-196`); the test binary now adds its own bundle (P2.T1). In `smoke_test.py` remove the three `metadata*` entries from `EXPECTED_OFX_BUNDLES` (`:217-219`) and from the `bundles` dict (`:306-308`), and check anything else that counts the shipped bundles or asserts the shipped set.
  - verify: `bash -n tools/ci/local/test.sh`; `python3 -I -m py_compile tools/ci/smoke_test.py`; at the phase build, `tools/ci/local/test.sh ctest` runs without `build/assets/TestPlugins`, and `python3 tools/ci/smoke_test.py` passes against the phase AppImage.
  - size: M

- [ ] M140.P4.T4 — Workflow cache paths
  - files: `.github/workflows/ci.yml`, `.github/workflows/remote-build.yml`, `.github/workflows/release.yml`, `.github/workflows/nightly.yml`
  - approach: delete the `build/assets/TestPlugins` line from each "Cache test assets" `path:` list (`ci.yml:140`, `remote-build.yml:83`, `release.yml:83`, `nightly.yml:115`). Mechanical. The cache key hashes `fetch-assets.sh`, which P4.T2 changes, so the caches rebuild once on their own.
  - verify: `git grep -n TestPlugins -- .github` finds nothing; the YAML still parses (`python3 -I -c "import yaml,sys; [yaml.safe_load(open(f)) for f in sys.argv[1:]]" <the four files>`).
  - size: S

**Verification gate:**
- A fresh `tools/ci/local/fetch-assets.sh` run builds no openfx-metadata plug-in; `build/assets/Plugins/` holds only Arena, CImg, IO and Misc and there is no `build/assets/TestPlugins/`.
- Full build and ctest pass on a clean build dir, including the three new native-node gtests, the OFX test-plug-in gtests and the repointed `Metadata_Test` and `OfxMetadataBridge_Test` with the same case counts as before.
- `getPluginIDs()` has the three native nodes (grouped under Metadata) and none of the three `org.openfx.examples.metadata*` IDs, even with stale bundles on the path.
- `python3 tools/ci/smoke_test.py` passes against the phase AppImage.
- An Xvfb check: the tab menu's Metadata folder lists the seven native metadata nodes and no OFX example; a CompareMetadata node over two Read nodes with different attributes shows its differences, and the screenshot is shared with the user before sign-off.
- CI green on the PR.

## Decisions

- 2026-10-28: phases 1–4 implemented (builds pending). CompareMetadata compares by displayed text; PassiveKnobUpdate shared by both display nodes; CompareMetadata outputs A's metadata. Test-only OFX bridge plug-ins (natronTestMetadata, raw OFX C API, host via OfxPlugin::setHost, grouped under Other) replace openfx-metadata. PM call: no Engine/OfxRetired markers for the three org.openfx.examples.metadata* IDs (a marker means the bundle still ships; no backward compatibility) — fetch-assets deletes stale copies.
- 2026-10-19 — **Native Contribute namespace**: the demo keys move to `fr.natron.ContributeMetadata.*` (the native node ID), and the string-vector `tags` becomes one comma-joined string because `ImageMetadata` has no string vector. The OFX test plug-in keeps the `org.openfx.examples.metadataContribute.*` prefix so the bridge tests' key lists stay as they are.
- 2026-10-19 — **Whole openfx-metadata build goes**: with Compare, Contribute and TimeCode native and Print replaced by the in-tree test plug-in, nothing needs the fork's examples, so the pin, clone, build and `TestPlugins/` are all removed rather than trimmed.
- 2026-10-19 — **Test plug-in uses the raw C API**: the in-repo OpenFX tree has no metadata C++ wrapper (`ofxsMetadata.h` lives only in the openfx-metadata fork), so the bundle is plain C-style code against `libs/OpenFX/include`, and doubles as direct coverage of the suite entry points.
