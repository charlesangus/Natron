# M643 - AdjustBBox Zero Pixels Fix

On main (6befcdadb7, release AppImage), `Tests/gui/m121_routing_mix.py`'s `section_adjustbbox` (~:271) fails one check. After Num Pixels goes from (1200, 800) to (0, 0), the viewer centre stays red: the grown region of definition is never given back. `Engine/Nodes/Transform/AdjustBBox.cpp` hasn't changed since M136 - Edge Policy Investigation (3bf923b042), so something around it regressed, most likely RoD or frame caching or the identity path at zero padding. Bisect main for the change, fix the cause, and add a headless regression that PR CI runs, since PR CI runs no GUI scripts.

## Scope

**In:**
- Bisecting main for the first commit where the m121 adjustbbox check fails.
- Fixing the root cause, in AdjustBBox or in the shared code that regressed.
- A ctest regression that changes Num Pixels to 0 on a live node and checks the RoD and the pixels.

**Out:**
- Adding GUI scripts to PR CI (`ci.yml` has no GUI leg).
- Other m121 sections, unless the bisected cause breaks them too.

**Edge cases:**
- If the cause is shared (RoD cache, identity), other nodes whose knob returns them to identity may be hit too. Note any you find, but fix only the shared cause.

**Assumptions:**
- The failure reproduces on a local release build under Xvfb, not only on the AppImage.

## Phase 643.1: Bisect

- [ ] M643.P1.T1 — Find the commit that broke Num Pixels 0
  - files: none changed; `Tests/gui/m121_routing_mix.py`, `build/deeprepro/run-gui.sh`, `tools/ci/remote/build.sh`
  - approach: confirm the failure with m121 on main, then bisect main's first-parent history between 06f3aba937 (M121 - Native Routing And Mix, where it passed) and 6befcdadb7. Use remote `release --gui "Tests/gui/m121_routing_mix.py"` gates per probe (cap ~4 concurrent), or local Xvfb runs if a release build is cheaper. Record the first bad commit and the subsystem it touched as a milestone decision.
  - verify: the decision names a first-bad commit, and m121 passes on its parent and fails on it
  - size: M

## Phase 643.2: Fix

- [ ] M643.P2.T1 — Fix the regression at its cause
  - files: decided by P1.T1; start from `Engine/Nodes/Transform/AdjustBBox.cpp` and the code the first bad commit changed
  - approach: fix the cause the bisect found, not a workaround in AdjustBBox. If it's a shared cache or identity path, fix it there. Comments follow the comment policy.
  - verify: `Tests/gui/m121_routing_mix.py` passes in a release `--gui` gate
  - size: L
- [ ] M643.P2.T2 — Headless regression for padding back to zero
  - files: `Tests/Native/NativeAdjustBBox_Test.cpp` (the `NativeAdjustBBoxTest` fixture, ~:126); add a test elsewhere too if the cause is shared
  - approach: on one live node, render with Num Pixels (1200, 800) and Stretch edge mode, set it to (0, 0), render again through the same path the viewer uses (including the cache P1.T1 implicated), and assert the RoD equals the source's and the pixel outside the source is empty. It must fail on the first bad commit. Bundled gtest is old: no `GTEST_SKIP`, `INSTANTIATE_TEST_CASE_P` only, no `ASSERT_TRUE(ptr)`.
  - verify: `ctest -R NativeAdjustBBox` passes at the phase build and fails with P2.T1 reverted
  - size: M

**Verification gate:** `ctest -R NativeAdjustBBox` green; release `--gui "Tests/gui/m121_routing_mix.py"` gate green; full debug PR CI green.
