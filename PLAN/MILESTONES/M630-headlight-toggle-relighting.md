# M630 - Headlight Toggle Relighting

Turning the per-pass headlight off leaves path-traced renders unlit. Main's per-pass headlight toggles scene lights on the live engine (`Engine/Nodes/Scene/HydraRenderImpl.cpp`, live `applyHeadlight(false)` ~:3949). Switching the headlight on works. Switching it back off leaves Cycles rendering the stage with no light (cube red 0). The explicit-off path at ~:1866 remakes the engine and is fine. Main's tests never switch scene lights back on. M365 - Delegate Display Transform works around it on its branch by remaking the engine on any headlight change. Find the cause (UsdImaging light pruning, Hydra's built-in lights, or Natron's draw filter), then fix the live path or adopt the remake on main.

## Scope

**In:**
- The headlight on→off transition on a live engine, for every bundled delegate (Storm, Cycles, MoonRay, Nucleon Utility).

**Out:**
- M365 - Delegate Display Transform's lighting table and exposure work.

**Edge cases:**
- **Stage with no lights:** headlight off then shows black. That is correct, and the test must use a stage with a key light.
- **M365 already remakes the engine:** if M365 lands first, this milestone keeps its remake and adds only the test and the root-cause note.

## Phase 630.1: Diagnose and fix

- [ ] M630.P1.T1 — Find why scene lights stay off after the headlight is switched off
  - files: read-only: `Engine/Nodes/Scene/HydraRenderImpl.cpp` (~:1866, ~:3949 `applyHeadlight`), the draw filter, UsdImagingGL engine light setup; notes to `build/m630/diagnosis.md`
  - approach: trace the on→off sequence on a live Cycles engine: which lights the scene index or the render delegate holds before and after, and whether the scene lights are re-added. Check UsdImaging's light pruning flag, the built-in default light, and Natron's draw filter. Compare with the remake path at ~:1866.
  - verify: the notes name the cause with file:line and propose either a live fix or a remake on every headlight change, with its cost (one engine rebuild per toggle)
  - size: L
- [ ] M630.P1.T2 — Fix the live path, or remake the engine on any headlight change
  - files: `Engine/Nodes/Scene/HydraRenderImpl.cpp`; `Tests/Scene/USDRenderStorm_Test.cpp` (~:1796), the Cycles render tests in `Tests/Scene/`
  - approach: apply T1's chosen fix. Add a test per delegate that renders with a key light, headlight on, then headlight off, and checks the cube is lit by the key light (a colour channel above a threshold), not just dimmer.
  - verify: the new off-after-on test fails on main and passes with the fix, for Cycles at least; `ctest -R "USDRender|CyclesDelegate"` green
  - size: M

**Verification gate:** full build and ctest green; the on→off→on sequence renders lit in every bundled delegate in the tests.
