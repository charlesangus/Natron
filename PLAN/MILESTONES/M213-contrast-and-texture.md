# M213 - Contrast And Texture

A native `ContrastTexture` node ported from darktable master's new `src/iop/contrastntexture.c` (about 730 lines, no OpenCL). It adds local contrast and clarity across several detail levels, built on the exposure-invariant guided filter (EIGF) from M209 - Edge-Aware Filter Library. It is darktable's scene-referred replacement for the Lab "local contrast" module (bilat), which is excluded as display-referred. The module is not in the 5.6.2 release that `DARKTABLE_REF` pins, so it is ported from a separate darktable master pin (see `## Decisions`).

Every user-facing control and darktable default is ported (`port-means-every-control`).

Deps: M209 - Edge-Aware Filter Library. See `DECISIONS/2026-10-25-darktable-ai-and-image-tool-ports.md`.

## Scope

**In:**
- **`ContrastTexture`** in `Engine/Nodes/Filter/`, plain ID, major 1. Inputs: Source and an optional Mask (the host MaskMix).
- **Every control** of the module at the pinned SHA. P1.T1 lists them from its params struct and GUI.
- **A GPU path** on M209's EIGF kernels. darktable has none, so the CPU port is the reference.
- **darktable reference parity** from a second darktable build at the master pin.
- **A params-to-knob audit**, a doc, an icon.

**Out:**
- darktable's bilat (local Laplacian, Lab): not ported.

**Edge cases:**
- **Proxy scale:** detail-level radii are relative to the image size, so proxies match.
- **Region-of-interest expansion:** by the largest level's EIGF radius.
- **Values above 1 and negative values:** EIGF works on log luminance with a floor. Negatives pass through scaled by the gain.

**Assumptions:**
- The module is stable on master at the pinned SHA. If darktable 5.8 has been released by the time this milestone starts, pin the 5.8 tag instead.

## Phase 213.1: Node

- [ ] M213.P1.T1 — Pin the master SHA and extend the reference harness
  - files: `tools/ci/local/fetch-assets.sh`, `tools/raw/make-reference.sh`, `tools/raw/make-reference.py`
  - approach:
    - Add `DARKTABLE_MASTER_REF`, a darktable master SHA that contains `contrastntexture.c` (as of 2026-10-24, `fee19b421`), or the 5.8 tag if it has been released.
    - Teach the harness a per-job `ref`, so jobs for this module build and use a second darktable-cli image at that pin. Every M80 job stays at `DARKTABLE_REF`.
    - List the module's params fields and GUI controls in the job comments.
  - verify:
    - `make-reference.sh --dry-run --only contrastntexture` lists the new jobs with the master ref.
    - A real run renders them on a host that can pull.
  - size: M

- [ ] M213.P1.T2 — ContrastTexture node (CPU)
  - files: new `Engine/Nodes/Filter/ContrastTexture.h`, `Engine/Nodes/Filter/ContrastTexture.cpp`, `Engine/Nodes/Filter/ContrastTexture.reg.inc`
  - approach: port `contrastntexture.c`'s process and its detail-level decomposition on M209's `Eigf`, with every control and default.
  - verify: P1.T4's reference jobs pass.
  - size: L

- [ ] M213.P1.T3 — GPU path
  - files: new `Engine/Gpu/Kernels/contrastntexture.slang`, `contrastntexture.cmake`, `Engine/Nodes/Filter/ContrastTexture.cpp`, new `Tests/Native/NativeContrastTextureGpu_Test.cpp`
  - approach: compose M209's GPU EIGF with a per-level gain kernel.
  - verify: `ctest -R NativeContrastTextureGpu` passes on lavapipe within 1e-4 relative of the CPU path.
  - size: M

- [ ] M213.P1.T4 — Reference parity, knob audit and docs
  - files: `Tests/fixtures/raw/reference/` (new), new `Tests/Native/NativeContrastTexture_Test.cpp`, `Engine/Nodes/Filter/docs/ContrastTexture.md`, `Gui/Resources/Images/NativeNodes/`
  - approach: compare against P1.T1's references on the same pre-module image in linear Rec.2020, run the params audit, and run identity checks. Write the doc with the tolerance and add an icon.
  - verify: `ctest -R NativeContrastTexture` passes within the doc's `Tolerance:`.
  - size: M

## Decisions

- 2026-10-25 — **Master pin for one module:** `contrastntexture` exists only on darktable master (the 5.8 draft), and bumping `DARKTABLE_REF` would invalidate every M80 - Native RAW Support reference. So this module gets its own `DARKTABLE_MASTER_REF` and a second harness image.

**Verification gate:**
- The full build and ctest pass, including both ContrastTexture suites.
- Under Xvfb, ContrastTexture renders on a plate at several detail settings, and screenshots against darktable's render are shared with the user before sign-off.
- An AppImage built with `package.sh` is handed over for UAT.
