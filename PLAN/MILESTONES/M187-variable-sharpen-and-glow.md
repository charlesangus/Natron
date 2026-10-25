# M187 - Variable Sharpen And Glow

Exact I-variants of M123 - Native Sharpen And Glow's nodes (user, 2026-10-24): **ISharpen** and **IGlow**, and **IBloom** since Bloom shares Glow's structure. Each base node is built from a blur of its input; the I-variant swaps that blur for M184 - Variable-Size Filter Kit's variable-radius IBlur kernel (`VariableBlurKernels`) with radius `lerp(sizeAtBlack, size, Map)`, and also scales the effect's amount by the Map, so Map black is exactly the input.

## Scope

**In:**
- ISharpen, IGlow and IBloom as I-variant registrations of the base classes, with the Map convention, CPU and GPU paths, tests, icons and docs.

**Out:**
- The base nodes: M123 - Native Sharpen And Glow.

**Edge cases:**
- Map scales both the radius and the amount, so the transition from no effect is smooth (radius alone would leave a hard sharpen edge at tiny radii); a `mapDrives` choice (Size and amount / Size only / Amount only) lets the user pick, default Size and amount.
- Glow's highlight threshold is not Map-driven.

## Phase 187.1: Nodes

- [ ] M187.P1.T1 — Write ISharpen (CPU and GPU)
  - files: `Engine/Nodes/Filter/Sharpen.cpp`, `Engine/Nodes/Filter/ISharpen.reg.inc`, `Engine/Gpu/Kernels/sharpen.slang`, `Tests/Native/NativeISharpen_Test.cpp`
  - approach: A template or mode flag on Sharpen's class selects the variable blur (`VariableBlurKernels` on CPU, `iblur.slang`'s passes on GPU) and multiplies the amount by the Map per `mapDrives`. Register `NATRON_I_VARIANT(Sharpen, ISharpen, …)`.
  - verify: tests: Map 0 identity; Map 1 matches Sharpen within IBlur's stated tolerance; GPU parity on lavapipe within 1e-4 relative.
  - size: L

- [ ] M187.P1.T2 — Write IGlow and IBloom (CPU and GPU)
  - files: `Engine/Nodes/Filter/Glow.cpp`, `Engine/Nodes/Filter/Bloom.cpp`, `Engine/Nodes/Filter/IGlow.reg.inc`, `Engine/Nodes/Filter/IBloom.reg.inc`, `Tests/Native/NativeIGlow_Test.cpp`
  - approach: As T1, for Glow's and Bloom's blur stages.
  - verify: tests: Map 0 identity; Map 1 matches the base within tolerance; GPU parity on lavapipe within 1e-4 relative.
  - size: L

- [ ] M187.P1.T3 — Icons, cost seeds and docs
  - files: `Gui/Resources/Images/NativeNodes/`, `Engine/Nodes/Filter/*.costseed.inc`, `Engine/Nodes/Filter/docs/{ISharpen,IGlow,IBloom}.md`
  - approach: Per the native checklist; docs describe `mapDrives`.
  - verify: `ctest -R NativePluginList` passes; docs reviewed at the gate.
  - size: S

## Decisions

- 2026-10-25 — **Map drives size and amount by default, with a `mapDrives` knob (user).

**Verification gate:** the full build and ctest pass, including the new suites (GPU on lavapipe and RADV). Under Xvfb ISharpen and IGlow with a roto Map are rendered; screenshots shared with the user before sign-off; an AppImage built with `package.sh` is handed over for UAT.
