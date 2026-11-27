# M635 - Layered Flatten Clipping And Groups

When a PSD or KRA has no stored composite, the flattened composite that M79 - Layered Document Readers synthesizes (`Engine/Nodes/IO/LayeredDocs/LayeredDocument.cpp:381`, `flatten()` :562) ignores clipping masks and group isolation. It multiplies `opacity * groupOpacity` and checks `groupVisible` per layer, which is pass-through for every group. It never reads `clipping`, which only `KraInput.cpp:757` sets. Only Normal-over is documented. PSD and Krita clip differently, so this starts with a design task.

## Scope

**In:**
- Clipping masks and group isolation (isolated vs pass-through) in the synthesized flatten, for PSD and KRA, each following its own application's rules.

**Out:**
- Blend modes beyond those M79 already supports.
- Layer effects (PSD lfx, Krita layer styles).

**Edge cases:**
- **Clipped layer whose base is hidden:** follows each app's rule (in Photoshop the clipped layer hides too).
- **Clipping inside an isolated group:** the base is the lowest layer of the run inside that group.
- **Stored composite present:** still used as-is; this only changes the synthesized path.

## Phase 635.1: Design

- [ ] M635.P1.T1 — Specify PSD and Krita clipping and group isolation rules
  - files: read-only: `Engine/Nodes/IO/LayeredDocs/LayeredDocument.cpp`, `PsdInput.cpp`, `KraInput.cpp`; Krita and PSD format references online; design to `build/m635/flatten-design.md`
  - approach: write the composite algorithm for each format: how a clipping run composites onto its base, which group blend mode means isolated (PSD: any non-pass-through group; Krita: its passthrough flag), and how opacity applies inside versus outside an isolated group. Note where PSD and Krita differ. Read which flags the readers already parse, and which they would need to.
  - verify: the design gives the algorithm, the per-format differences, and the reader fields needed, and the PM confirms it with the user before P2
  - size: L

## Phase 635.2: Implement

- [ ] M635.P2.T1 — Parse clipping and group pass-through flags in both readers
  - files: `Engine/Nodes/IO/LayeredDocs/PsdInput.cpp`, `KraInput.cpp`, `LayeredDocument.h` (layer model); `Tests/Native/NativeReadLayeredDocs_Test.cpp`, `NativeReadKra_Test.cpp`
  - approach: per the design, read PSD's clipping byte and group blend mode, and Krita's clipping and passthrough flags, into the layer model.
  - verify: reader tests assert the flags on new small fixtures in `Tests/fixtures/layered/` (≤256 px, one clipping run and one isolated group per format)
  - size: M
- [ ] M635.P2.T2 — Composite clipping runs and isolated groups in flatten
  - files: `Engine/Nodes/IO/LayeredDocs/LayeredDocument.cpp` (`flatten` :562, the synthesis at :381); `Tests/Native/NativeReadLayeredDocs_Test.cpp`, `LayeredDocsSeam_Test.cpp`
  - approach: implement the design as a recursive flatten: an isolated group composites into its own buffer before blending onto its parent; a clipping run composites onto its base's alpha. Compare against each fixture's stored composite (saved by the app, then stripped for the synthesized test).
  - verify: flatten of each fixture matches its app-saved composite within 1/255 per channel
  - size: L

**Verification gate:** full build and ctest green; the fixtures' flattened results match the app composites; Xvfb screenshot of a ReadPSD flatten next to the stored composite sent to the user.
