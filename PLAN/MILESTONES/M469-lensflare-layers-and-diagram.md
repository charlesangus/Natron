# M469 - LensFlare Layers And Diagram

Expose realflare's outputs on LensFlare: RGBA carries the comp (ghosts plus starburst, alpha 0); extra layers `flare`, `starburst`, `ghost`, `ghostAperture` and `starburstAperture` render only when something downstream requests them; and an `output` choice swaps RGBA to any of them or to the lens Diagram — a cross-section of the lens with the traced ray paths of one ghost. Ported from realflare's layer list (`api.Layer`) and `shaders/diagram_{lens,rays}.frag`.

## Scope

**In:**
- `output` choice: Comp (default), Flare, Starburst, Ghost, Ghost Aperture, Starburst Aperture, Diagram.
- Layers `flare`, `starburst`, `ghost` (the diffracted ghost image), `ghostAperture`, `starburstAperture`, each produced only when requested (layer-aware render).
- Diagram knobs: `diagramDivisions` (8), `diagramSize` (2048 × 1024); the diagram uses `ghost` from the debug knobs.
- CPU and GPU for every output (the diagram on the CPU only).

**Out:**
- Per-ghost layers, depth, an alpha matte: none (realflare has none).
- The diagram in the lens browser: M463 - Bundled Lens Database draws its own cross-section.

**Edge cases:**
- Output = Diagram: the node's RoD becomes `diagramSize` at the origin, ignoring the generator's extent.
- Aperture outputs: shown at their own resolution, centred in the format.
- A layer requested on a render that doesn't need the comp: only that layer's work runs (no ghost raster for `starburstAperture`).

## Phase 469.1: Layers and diagram

- [ ] M469.P1.T1 — Output choice and layers
  - files: `Engine/Nodes/Generator/LensFlare.{h,cpp}`, `Tests/Native/NativeLensFlare_Test.cpp`, `Tests/Native/NativeLensFlareGpu_Test.cpp`
  - approach: Declare the five layers as produced layers and render each on request from the existing ghost, starburst and aperture stages (CPU and GPU); `output` routes the chosen one to RGBA. Follow an existing native multi-layer producer for the layer declaration (find one with the layer registry, `Tests/LayerRegistry_Test.cpp`).
  - verify: gtest: Comp = flare + starburst within 1e-6; requesting only `starburstAperture` runs no ghost raster (counter); each layer matches its Output choice; GPU parity for each.
  - size: M

- [ ] M469.P1.T2 — Lens diagram
  - files: `Engine/Nodes/Generator/LensFlareDiagram.{h,cpp}` (new), `Engine/Nodes/Generator/LensFlare.cpp`, `Tests/Native/LensFlareGhosts_Test.cpp`
  - approach: Port `diagram_lens.frag` and `diagram_rays.frag` on the CPU: draw each surface's profile from M462's `layout()` and the glass between surfaces, then trace the selected ghost at `diagramDivisions` along one axis and draw each ray segment between recorded intersections, coloured by wavelength; antialiased lines into a float image of `diagramSize`.
  - verify: gtest: the diagram of the Double-Gauss shows the stop at its axial position (a known pixel column is dark); changing `ghost` changes the ray image; deterministic across runs.
  - size: M

- [ ] M469.P1.T3 — Docs, goldens and audit rows
  - files: `Engine/Nodes/Generator/docs/LensFlare.md`, `Tests/Native/goldens/LensFlare/` (diagram crop), `docs/lensflare/controls-audit.md`
  - approach: Document outputs and layers; a diagram golden; fill the audit's output and diagram rows.
  - verify: `ctest -R NativeLensFlare` passes; the audit has no MISSING rows left.
  - size: M

**Verification gate:** build and ctest green with GPU parity; under Xvfb, screenshots of each Output choice shared with the user (collected at F31 - Lens Flares' UAT).

## Decisions

- 2026-11-16 — **Layers plus an Output choice (user, /cat-plan grill):** RGBA is the comp; components as extra layers on request; Output swaps RGBA to any of them or to the diagram.
