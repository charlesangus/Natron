## Pending

### 2026-10-24T20:30:00-04:00 — info
- refs: none

Found in M167 - Per-Frame Node Metadata: `Node::refreshDirtyHashes` can hand back a hash computed over a still-dirty input. After 886c0baf1 such a node stays dirty, but only the metadata cache checks `getHashValueIfUpToDate` before storing. The identity cache, the actions cache (RoD etc.) and the image cache still store under such a hash during concurrent edits. `NativeEffectBase::getOutputMetadata` (key/value metadata) also has no mark-count or write-gate check, so a hash that goes A→B→A during a derive can leave a stale entry. Not owned by any milestone yet.

### 2026-10-24T19:40:00-04:00 — info
- refs: none

Python bindings that return `StatusEnum` and can set a Python error from C++ (e.g. via `checkIsEditable`/`checkHolderIsEditable` on a placeholder node for an uninstalled plug-in), such as `ParametricParam.addControlPoint`/`setNthControlPoint` and other param setters, surface the error as `SystemError: ... returned a result with an exception set` instead of the intended exception, because the generated wrapper keeps converting the return value. M96 fixed this for its two new weighted calls with typesystem inject-code (`if (PyErr_Occurred()) return {};`). A sweep over the other `StatusEnum` methods that can raise is a follow-up, not owned by any milestone yet.

### 2026-10-24T19:30 — info
- refs: none
- `Tests/gui/m55_node_graph.py` section `splice-vertical` fails 6 checks on main (seen on `M80-final.AppImage`, main plus M80, and on M134's branch): the hover highlight isn't drawn, the splice connections aren't made, and redo doesn't re-apply them. Found while gating M134 - Output Picker Pixel Aspect, which doesn't touch the node graph. Needs an owner: likely a regression from a node-graph change, or a script assumption that no longer holds.

### 2026-10-24T19:40 — info
- refs: none
- `NativeRawColorCalibrationGpuTest.EveryAdaptationVersionClipAndGamutMatchesTheCpu` (M80 - Native RAW Support, on main) fails on real RADV hardware (RX 7900 XTX, `NATRON_TEST_REQUIRE_GPU=1`): in several adaptation/clip/gamut cases one float of 256 (float 26) differs from the CPU by ~1.3–1.6e-6, against an expected 0 mismatches (`Tests/Native/RawGpuTestSupport.h:202`). It passes on lavapipe. It needs a ulp- or relative-tolerance bound like the other GPU parity suites. It could go in M171 - GPU Correctness Fixes, or to whoever owns RAW follow-ups. Found during M110 - Remaining Native GPU Ports' RADV run; log at the gpu PM's scratchpad `m110-radv-ctest.log`.
