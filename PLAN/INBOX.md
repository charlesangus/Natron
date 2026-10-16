## Pending

### 2026-10-16T09:00:00Z — change-request
- refs: none
- New milestone requested (user accepted the miss in M90 - Render Path Serial Overhead): **Blur In-Graph Overhead** (E6). In-graph native Blur at UHD on 16 threads is 3.1× its bare kernel at FIR σ3 (4.70 vs 1.51 ms/Mpx) and 2.0× for IIR (6.83 vs 3.37). Profile on the 16-thread host (symbolised build) and cut the fixed per-task overhead (fill, `writeWindow` finish, planar buffer allocation) and the padded-region cost at large σ. Data: `M90-render-path-serial-overhead.md` Decisions 2026-10-16. Deps: M90.


### 2026-10-16T02:40:00-04:00 — change-request
- refs: M110
- When M109 - GPU Region Engine Hardening was elaborated, it moved multi-plane placement candidates, which Shuffle-family nodes need to join regions, out of its scope and into M110 - Remaining Native GPU Ports. The work needs a multi-plane `GpuQuery`/`GpuRenderArgs` in `Engine/Nodes/Image/GpuRender.h`, which M126 - Runtime GPU Shaders edits. M110's stub says it is blocked on "M109's multi-plane placement". Make that a task of M110's own instead, starting after M126 merges, and add M126 to M110's Deps.

### 2026-10-16T12:30:00Z — change-request
- refs: none
- New milestone requested (user): **Native Metadata Utilities** (E3, after M77 - Native Metadata Nodes). Native versions of the OFX example metadata nodes that M77 left without a native equivalent — `org.openfx.examples.metadataCompare`, `metadataContribute` and `metadataTimeCode` — then retire the OFX ones from the bundle and the asset fetch. Also replace the three OFX example plug-ins the test suite uses as metadata-bridge fixtures (Contribute, Print, TimeCode in `build/assets/TestPlugins/`) with an in-repo test-only OFX plug-in, so the bridge keeps coverage once they're gone. Deps: M77.

