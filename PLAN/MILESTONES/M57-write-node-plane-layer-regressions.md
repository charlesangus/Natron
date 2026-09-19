# Milestone 57: Fix Write node plane/layer regressions found while testing M39

M39 renamed "plane" to "layer" everywhere on the Natron side but correctly left the OFX ABI
boundary untouched. Testing the rename surfaced two bugs on the **Write** node that trace back
to that boundary: `WriteNode::getCreateChannelSelectorKnob()` returns `false`
(`Engine/WriteNode.cpp:970`), so Natron never creates its own host-level `processAllLayers`/"All
Layers" knob for Write — the only "process everything" checkbox a user sees on a Write node is
the **embedded OFX writer plugin's own** `kMultiPlaneProcessAllPlanesParam` ("All Planes"),
defined in `SupportExt/ofxsMultiPlane.h` inside the `charlesangus/openfx-io` fork (fetched by
`tools/ci/local/fetch-assets.sh`, built into `build/openfx-io-fork/`) — a separate repository
M39 never touched. This repo's own allowlist grep is clean; there is nothing left to rename here.

- [x] M57.P1.T1 — Rename the embedded writer's "All Planes" checkbox to "All Layers"
  - files: (in the `charlesangus/openfx-io` fork, not this repo) `SupportExt/ofxsMultiPlane.h`
    (`kMultiPlaneProcessAllPlanesParam`, `…ParamLabel`, `…ParamHint`), `SupportExt/ofxsMultiPlane.cpp`
    (matching comment ~line 660); this repo's `tools/ci/local/fetch-assets.sh` (`OPENFX_IO_REF`)
  - approach: clone/worktree `charlesangus/openfx-io`, rename
    `kMultiPlaneProcessAllPlanesParam "processAllPlanes"`→`"processAllLayers"`, its label `"All
    Planes"`→`"All Layers"`, and the hint text's "plane"→"layer" wording — same clean-break, no
    compatibility shim as M39's decision (`.ntp` files with a Write node's old `processAllPlanes`
    value will fail to load that one param; accepted). Scope is `openfx-io` only — `openfx-misc`
    and `openfx-arena` vendor their own copies of `ofxsMultiPlane.*` and are out of scope unless a
    follow-up finds the same string on a non-Write node. Commit, push, open a PR against
    `charlesangus/openfx-io`, then bump `OPENFX_IO_REF` in `tools/ci/local/fetch-assets.sh` to the
    merged commit.
  - verify: `tools/ci/local/fetch-assets.sh` re-fetches at the new ref; `tools/ci/local/build.sh
    debug --reconfigure && tools/ci/local/test.sh ctest debug && tools/ci/local/test.sh smoke
    debug` green; under Xvfb, a Write node's params panel shows "All Layers", not "All Planes";
    `grep -rn 'processAllPlanes\|All Planes' <openfx-io checkout>/SupportExt/ofxsMultiPlane.*`
    prints 0 lines.
  - size: M
- [ ] M57.P1.T2 — Diagnose and fix Write not writing all layers when "All Layers" is checked
  - files: unknown until diagnosed — candidates are the `charlesangus/openfx-io` fork's
    `IOSupport/GenericWriter.cpp` / `SupportExt/ofxsMultiPlane.cpp` (plugin-side render and
    `getClipComponents` logic) and this repo's `Engine/OfxImageEffectInstance.cpp`,
    `Engine/OfxClipInstance.cpp`, `Engine/EffectInstance.cpp` (host-side layer enumeration that
    M39.P3–P4 renamed: `getUserLayers`, `getPassThroughForNonRenderedLayers`,
    `getComponentsNeededAndProduced`, `getThreadLocalRenderedLayers`)
  - approach: reproduce first — build a source with 2+ named layers upstream of a Write pointed
    at `.exr` (WriteOIIO supports multi-part/multi-layer output), check "All Layers", render, and
    inspect the output with `oiiotool -info -v` against the input's layer list. Bisect against a
    pre-M39 commit (`3e14c2a1e` or earlier on `main`) to confirm this is an M39 regression and not
    pre-existing; if pre-existing, stop, record that as a decision, and rescope. If it is a
    regression, the mechanical rename across `EffectInstance`/`OfxImageEffectInstance`/
    `OfxClipInstance` is the prime suspect — trace the plugin's `processAllLayers`-checked render
    request through the OFX `getClipComponents` action into whichever renamed Natron-side function
    now drops or mis-enumerates the requested set, and fix that one call site.
  - verify: the repro above writes every layer present at the input to the output file (the
    output's subimage/channel list matches the input's layer list); full `tools/ci/local/test.sh
    ctest debug` and `tools/ci/local/test.sh smoke debug` green; confirmed under Xvfb.
  - size: L

**Verification gate:** both tasks' `verify` steps pass; a Write node's checkbox reads "All
Layers" and, when checked, writes every layer present at its input.

## Decisions

- 2026-09-19 — M57.P1.T1 landed as `charlesangus/openfx-io` commit `87264e5` on branch
  `fix/all-planes-to-all-layers` (PR #3, left open unmerged): matches the standing pattern
  from `56b782a4a` (PR #2 also open) — `OPENFX_IO_REF` pins directly to the fork's branch tip
  rather than waiting on a merge, since we control the fork.
