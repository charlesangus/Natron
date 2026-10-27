# M255 - Delegate Output Spikes

Two timeboxed spikes on the working Cycles and MoonRay delegates that settle what the
remaining delegate work depends on: (1) how a USDRender chooses its AOVs and light-path
expressions, and (2) whether true deep output can be rendered in-process, without going out
to an external file or process the way Nuke's external-renderer bridges do. The output is
findings and a decision, not shipped features; the stubs M256 - Delegate AOV Selection, M257 -
Cycles Deep Output and M258 - MoonRay Deep Output are elaborated from them.

## Scope

**In:**
- AOV spike: build both candidate mechanisms on Cycles — (a) the delegate's discovered AOV list shown as a checklist plus custom LPE strings, and (b) AOVs declared in USD `RenderSettings`/`RenderProduct`/`RenderVar` prims — and compare authoring effort, delegate support and Natron layer mapping.
- Deep spike on both delegates: can the renderer produce per-sample deep data (z, zback, colour, alpha, ids) in memory, to a buffer our code owns, with no deep EXR file and no separate renderer process? Measure memory per pixel and render time against flat.
- A written recommendation per spike, recorded as decisions by the PM, and elaboration of M256 - Delegate AOV Selection, M257 - Cycles Deep Output and M258 - MoonRay Deep Output.

**Out:**
- Shipping either mechanism — M256 - Delegate AOV Selection, M257 - Cycles Deep Output, M258 - MoonRay Deep Output.
- Deep for Storm — none.
- Patching production code on the milestone branch: spike code stays on a throwaway branch.

**Edge cases:**
- A renderer can only produce deep through an on-disk file or a separate process: the spike records that as "not feasible in-process", with the measured alternative cost, and the user decides whether the file route is acceptable (it would break the foundation principle of no side channels).
- Neither renderer can: the deep stubs are cancelled and the result goes to the user.
- Hydra has no variable-length AOV: the spike must say whether a delegate patch (ours, maintained) or a render-buffer extension is needed, and the maintenance cost.

**Assumptions:**
- Cycles has no upstream deep output; Blender PR #154410 and the `Niik-l/cycles` branch `feature/cycles-deep` (read-only prior art, GPL-compatible with Cycles' Apache licence needing a check) implement it. Verify before relying on either.
- Unconfirmed: MoonRay supports deep output natively (its `RenderOutput` / `DeepBuffer` support). Verify in the source.
- "Without going out to a file" was read from the user's truncated answer (2026-10-27) as in-memory, in-process rendering of deep. Correct this if it meant something else.

## Phase 255.1: AOV mechanism

- [ ] M255.P1.T1 — Prototype delegate-discovered AOV checklist on Cycles
  - files: throwaway branch only; report
  - approach: query the Cycles delegate for its AOV descriptors (`GetDefaultAovDescriptor`, render-var tokens) and request a custom set through `SetRendererAovs`; render and read each. Record which AOVs and LPE strings it accepts and how they name their channels.
  - verify: report lists the AOVs obtained, names, formats, and any rejected request, with file/line citations to the delegate source.
  - size: M

- [ ] M255.P1.T2 — Prototype RenderSettings-driven AOVs on Cycles
  - files: throwaway branch only; report
  - approach: author `RenderSettings`/`RenderProduct`/`RenderVar` prims in a stage and see whether the delegate's render-product path honours them in `UsdImagingGLEngine`; read Solaris/Karma and usdview behaviour from `.local/docs`. Record what is needed on the Natron side to author them (a node).
  - verify: report states whether Cycles consumes the prims, with source citations, plus the extra node/authoring cost.
  - size: M

## Phase 255.2: Deep feasibility

- [ ] M255.P2.T1 — Cycles in-process deep
  - files: throwaway branch only; report
  - approach: apply Blender PR #154410 / `feature/cycles-deep` to our pinned Cycles on a scratch build, render a deep test scene, and find how to hand the sample buffers to host code through the delegate instead of writing an EXR. Measure bytes per pixel and time against flat. State what a delegate patch would look like.
  - verify: report includes a picture of a flattened result and sample-count statistics for the test scene, build logs of the scratch patch, and the patch size.
  - size: L

- [ ] M255.P2.T2 — MoonRay in-process deep
  - files: throwaway branch only; report
  - approach: inspect MoonRay's deep output (source citations); render a deep test scene through its API or delegate, and find whether the sample buffers are reachable in-process. Same measurements as the Cycles task.
  - verify: report with sample statistics and the answer to "reachable in-process without a file", with source citations.
  - size: L

- [ ] M255.P2.T3 — Findings and decision drafts
  - files: report only
  - approach: combine the four reports into one comparison and a recommended transport for deep (delegate-side sample-list buffer, render-buffer extension or file route), the AOV mechanism, and elaborated task lists for the three stubs. Name the user decisions needed.
  - verify: the report answers each Scope question in one line and recommends one option per fork.
  - size: M

**Verification gate:** the report answers both spikes with evidence; the PM records the decisions and elaborates M256 - Delegate AOV Selection, M257 - Cycles Deep Output and M258 - MoonRay Deep Output (or cancels the deep ones) with the user's approval; nothing from the spike branches is merged.

## Decisions

- 2026-10-27 — **Spike deep transport and AOV mechanism first (user):** deep transport is not decided in advance, and for AOV selection the user asked to spike the delegate-discovered checklist and the USD RenderSettings route before choosing.
