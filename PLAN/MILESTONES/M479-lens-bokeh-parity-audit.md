# M479 - Lens Bokeh Parity Audit

Prove that the traced defocus work matches or exceeds Higx Optics (`https://higx.net/optics/`, guide at `https://higx.net/optics/guide/`; user, 2026-11-16). This is a control-by-control and feature-by-feature audit plus a showcase render of each feature. **Higx is never named in the repo** (user, 2026-11-16: the same rule as Nuke): the audit goes to the user and into the F13 - ZDefocus Rework PR body only. Docs, tooltips, comments and committed files use neutral wording.

Deps: M292 - Traced Lens Bokeh, M294 - ZDefocus Engine Revisit, M473 - Lens Camera And Focus Controls, M474 - Traced Lens Creative Aberrations, M475 - Aperture Maps And Front Filter, M476 - Traced Anamorphic Bokeh, M477 - Lens Cross-Section Panel, M478 - Cinema And Vintage Lens Prescriptions.

## Reference inventory (read 2026-11-16 from the guide pages)

The audit must have a row for each, re-checked against the live guide at audit time:

- **Camera:** Lens (19 spherical designs), Format (Custom, Full Frame 3:2, Full Frame 16:9 4K, Super 35 16:9, Super 35 4-perf 3:2, APS-C 3:2, Micro Four Thirds 4:3, Medium Format 4:3, Alexa 65), Iris Radius (mm), Filmback Offset (mm), Sensor Size, Edge Field Angle, Lens Coverage.
- **Focus:** Focus (0 in focus, ±0.5), Focus Distance (m, log slider), Depth in Meters, Source Distance; depth Channel, Gain, Bias.
- **General:** General Size, Input Encoding (13 curves), Blend Back, Boost Highlights, Boost Gamma, Boost Rolloff, Vignetting (compensated, relative, absolute), Vignette Softness, Edges (repeat frame edge, transparent, repeat content edge).
- **Quality:** Quality (Draft to Extreme), Soften Internal Cutoff, Sample Softness, Dither and Dither Amount, Highlight Detail, Highlight Quality, Highlight Threshold, Highlight View (Final, Highlights Only, Base Only, Coverage).
- **Creative:** Impression and Impression Power, Coma, Astigmatism, Field Curvature, Zonal Ripple and Ripple Rings, Lobes, Lobe Count, Lobe Power, Lobe Angle, Lobes Face Center, Squeeze Factor, CA Green/Magenta, CA Red/Blue, Flip Internals, Breathing X and Y, the "Deakinizer" look.
- **Aperture and filter:** Iris Blades, Blade Rotation, Iris Size, Iris Softness, Aperture Map input, Map Fit, Aperture Rotation, Opening, Front Filter input, Filter Shape (round filter, square mattebox), Filter Size, Front Filter at Stop, Filter Parallax, Filter Shading, Front Map is Pupil Phase, Phase Strength.
- **Layers and depth:** Layers A, B and C with their own depth and depth maps, Is Glitter, Glitter Matte, Normalize Glitter, Layer C is Distortion and Distortion, occlusion (eclipsed bokeh), Depth Slicing and Slices (0 = auto), Depth Cut (Cut, Grow, Feather).
- **Other:** the lens cross-section in the panel (blocked rays red), interactive GPU previews, per-pixel coma, astigmatism and field curvature, overlapping highlights adding up, licence-free rendering (theirs stamps a banner when unlicensed).

## Scope

**In:**
- The audit table: their control or feature → our knob or feature, node, default → verdict (Matched, Exceeded, Not applicable, or Ruled out), and for each Ruled out, the decision that ruled it out.
- A showcase: one render per feature, as separate screenshots.

**Out:**
- Running Higx itself: closed source, no licence; the audit is against its documentation.
- Any committed file naming Higx.

**Edge cases:**
- A guide page that changed since 2026-11-16: audit the live version, and list new controls as new rows.
- A control with no counterpart and no ruling decision: a MISSING row, which fails the gate. The fix is a new milestone through /cat-plan, or a user decision to rule it out.

**Assumptions:**
- Unconfirmed: the expected rulings are Not applicable for Input Encoding and Blend Back (Natron is scene-linear), for the Quality, Soften, Sample Softness, Dither and Highlight Detail/Quality/View controls (rasterised kernels have no sampling noise), and for the normalised Focus slider and Source Distance (M473 - Lens Camera And Focus Controls decisions); and Ruled out for Layers, Glitter and the Distortion layer (user, 2026-11-16).

## Phase 479.1: Audit

- [ ] M479.P1.T1 — Control and feature audit
  - files: `build/parity/lens-bokeh-audit.md` (not committed)
  - approach: Re-read the guide pages (index, optical, workflow, lenses, creative, aperture, layers, hosts) and this file's inventory. For each item, find our counterpart in the merged F13 - ZDefocus Rework code and docs (knob script names, defaults), and write the verdict with evidence: a knob name, a test name or a decision. Lens library: list their 19 against our database ids. Share the file with the user (SendUserFile) and paste the table into the F13 PR body.
  - verify: every inventory item and every live-guide control has a row; there are no MISSING rows; every Ruled out row cites a decision.
  - size: M

- [ ] M479.P1.T2 — Feature showcase renders
  - files: `Tests/gui/m479_showcase.py` (new; neutral names, no product mentions)
  - approach: Under Xvfb, render a point-highlight scene with a depth ramp and a foreground card through ZDefocus with: a vintage swirl lens (Petzval or Helios-44), cat-eye at the corners, each creative term, each look, an RGB aperture map, a dirt filter, a square mattebox, Pupil Phase, a true anamorphic lens desqueezed, eclipsed bokeh behind a defocused card, and the cross-section panel. Save each as its own screenshot.
  - verify: the script passes; the screenshots are shared with the user one file each and collected for F13 - ZDefocus Rework's UAT.
  - size: M

**Verification gate:** the audit has no MISSING rows and every Ruled out row cites a decision; the audit and showcase are shared with the user, who OKs parity before F13 - ZDefocus Rework's UAT; no committed file names Higx (`git grep -i higx` on the Feature branch is empty).

## Decisions

- 2026-11-16 — **Match or exceed Higx Optics (user, /cat-plan grill):** "the traced defocus work needs parity with this closed-source project … ensure we're matching at least what they're delivering or that we exceed it."
- 2026-11-16 — **The audit is a report, never in the repo (user):** the same rule as Nuke.
- 2026-11-16 — **Layers, glitter and the rain-on-glass layer are out (user).**
