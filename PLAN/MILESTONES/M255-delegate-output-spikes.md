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

- [x] M255.P1.T1 — Prototype delegate-discovered AOV checklist on Cycles
  - files: throwaway branch only; report
  - approach: query the Cycles delegate for its AOV descriptors (`GetDefaultAovDescriptor`, render-var tokens) and request a custom set through `SetRendererAovs`; render and read each. Record which AOVs and LPE strings it accepts and how they name their channels.
  - verify: report lists the AOVs obtained, names, formats, and any rejected request, with file/line citations to the delegate source.
  - size: M

- [x] M255.P1.T2 — Prototype RenderSettings-driven AOVs on Cycles
  - files: throwaway branch only; report
  - approach: author `RenderSettings`/`RenderProduct`/`RenderVar` prims in a stage and see whether the delegate's render-product path honours them in `UsdImagingGLEngine`; read Solaris/Karma and usdview behaviour from `.local/docs`. Record what is needed on the Natron side to author them (a node).
  - verify: report states whether Cycles consumes the prims, with source citations, plus the extra node/authoring cost.
  - size: M

## Phase 255.2: Deep feasibility

- [x] M255.P2.T1 — Cycles in-process deep
  - files: throwaway branch only; report
  - approach: apply Blender PR #154410 / `feature/cycles-deep` to our pinned Cycles on a scratch build, render a deep test scene, and establish whether hdCycles can fill M286's `HydraDeepBuffer` (returned from `HdRenderBuffer::GetResource()`) in-process, and the patch that needs. Measure bytes per pixel and time against flat. State what a delegate patch would look like.
  - verify: report includes a picture of a flattened result and sample-count statistics for the test scene, build logs of the scratch patch, and the patch size.
  - size: L

- [x] M255.P2.T2 — MoonRay in-process deep
  - files: throwaway branch only; report
  - approach: inspect MoonRay's deep output (source citations); render a deep test scene through its API or delegate, and establish whether MoonRay's DeepBuffer can fill M286's `HydraDeepBuffer` in-process, and the patch that needs. Same measurements as the Cycles task.
  - verify: report with sample statistics and the answer to "reachable in-process without a file", with source citations.
  - size: L

- [ ] M255.P2.T3 — Findings and decision drafts
  - files: report only
  - approach: combine the four reports into one comparison: per delegate, whether it can fill `HydraDeepBuffer` and the patch size; the AOV mechanism (checking how each candidate carries per-AOV metadata such as Cryptomatte manifests, for M285 - Utility Renderer AOVs); and elaborated task lists for the three stubs. Name the user decisions needed.
  - verify: the report answers each Scope question in one line and recommends one option per fork.
  - size: M

**Verification gate:** the report answers both spikes with evidence; the PM records the decisions and elaborates M256 - Delegate AOV Selection, M257 - Cycles Deep Output and M258 - MoonRay Deep Output (or cancels the deep ones) with the user's approval; nothing from the spike branches is merged.

## Decisions

- 2026-11-03 — **Deep transport is decided (user, 2026-11-01, `PLAN/DECISIONS/2026-11-01-natron-utility-renderer.md`):** M286 - Utility Renderer Deep Output defines it — a delegate's `HdRenderBuffer::GetResource()` returns a host-defined `HydraDeepBuffer` (per-pixel counts, offsets, named float channels) that USDRender converts into `DeepImage`. P2.T1/T2 now only establish whether Cycles and MoonRay can fill it in-process and the patch each needs; P2.T3 no longer recommends a transport. The AOV spike is unchanged; M256 - Delegate AOV Selection now has Natron Utility as its first full client (M285).
- 2026-10-28: Source findings (no renders yet). Cycles 5.2.0 hdCycles offers only color, depth, normal, primId, instanceId (elementId gets a descriptor but no pass; other names dropped with "Unknown pass"); no LPEs; ~60 core pass types unmapped (would need a delegate patch). Cycles registers no renderSettings Bprim, so RenderSettings-driven AOVs would mean Natron mapping RenderVars itself. Cycles 5.2.0 has no deep output. MoonRay accepts named AOVs, lpe:<expr>, primvars:/shader: and parameters:moonray:* aovSettings; its core has an in-memory DeepBuffer but only writes deep EXR, and the bundled hd_moonray renders out-of-process via Arras (flat frames only) — in-process deep needs hd_moonray_debug (in-process RndrRenderer, currently not bundled) plus a patch exposing DeepBuffer samples. Blocked on the user: run the spikes as real builds (patched Cycles image, scratch branch)? access to Blender PR #154410 / Niik-l cycles branch? accept switching MoonRay to in-process rendering for deep?
- 2026-10-27 — **Spike deep transport and AOV mechanism first (user):** deep transport is not decided in advance, and for AOV selection the user asked to spike the delegate-discovered checklist and the USD RenderSettings route before choosing.
- 2026-11-02 — Audit: no spike code or report committed. The local branch has no upstream and points at M252's tip (73190fc59), with no M255 commits. Only the source-level findings in the 2026-10-28 note exist; no task's verify is met. Still blocked on that note's three user questions (audit).
- 2026-11-16 — **Unblocked (user):** the spikes run as real builds (patched Cycles image, throwaway scratch branch, never merged); Blender PR #154410 and `Niik-l/cycles` `feature/cycles-deep` may both be read and ported as prior art, sources recorded. Switching MoonRay to in-process rendering for deep is not yet decided: the user asked for its pros, cons and costs first, so P2.T2 starts after that answer.
- 2026-11-16 — P1.T1 done (report `spike/m255/REPORT-P1T1-aov-checklist.md` on `spike/m255-delegate-output`, 10b70a084). MoonRay deep transport is being costed: (a) in-process only for deep (consultant: about 10–15 d), (d) keep Arras and add a deep message (the user leans this way), (e) render deep EXR to a file and read it back (user: "for a production renderer that's not crazy"). The user decides after (d) and (e) are costed.
- 2026-11-16 — MoonRay deep costing (consultant, source-read; figures estimated, not measured). MoonRay already fills DeepBuffer on the Arras/mcrt progressive path (only PROGRESSIVE_FAST skips it), and hdMoonray can request deep through `parameters:moonray:*` AOV settings. Only batch (`RaasApplication::writeFinal`) writes deep EXR. Options: (a) in-process for deep, 10–15 d, +600–950 lines, no isolation; (d) a banded DeepFrame Arras message sent after the FINISHED frame, 15–22 d, 900–1,400 lines across moonray, mcrt_messages, mcrt_computation and hdMoonray (the middle two need forking), 0.3–3 s per 1080p frame; (e) the Arras render writes a deep EXR side output, 8–12 d, half-float AOVs, temp files; (e) batch, a second render, poor fit. Recommendation (d), banded. Awaiting the user's choice.
- 2026-11-16 — **MoonRay deep transport: (d), Arras carries deep in bands (user).** Project-wide decision `PLAN/DECISIONS/2026-11-16-moonray-deep-over-arras.md`. P2.T2 is narrowed to verifying (d) with a real render on the published image. It checks that the mcrt/Arras path fills DeepBuffer with `output_type` deep on a RenderOutput requested via `parameters:moonray:*`, that McrtFbSender copes with a deep output alongside the flat ones, that deep weighting stays correct across progressive passes, and the actual sample counts and band sizes for a test scene. It no longer evaluates in-process rendering.
- 2026-11-16 — P1.T2 done (`spike/m255/REPORT-P1T2-rendersettings-aovs.md`, bb8b25259). Neither hdCycles nor `UsdImagingGLEngine` maps RenderSettings, RenderProduct or RenderVar prims to AOVs, so the mapping is host-side, as husk does it. Recommendation: store AOVs as RenderVars and use the delegate-discovered checklist as the UI that authors them. Open for the user at P2.T3: union multiple products or add a Product knob; table knob or stopgap text field for custom primvar and LPE vars; whether to patch the Cycles fork (about 75 lines for `cycles:<pass>` plus an `aovNames` stat); whether to drop the dead elementId descriptor.
- 2026-11-16 — P2.T1 done (`spike/m255/REPORT-P2T1-cycles-deep.md`, 7b386b9f7). Cycles deep is feasible in-process. The pinned fork already has Niik-l's deep kernel, and a +194/−6 patch hands a `HydraDeepBuffer` out through `GetResource()` when `cycles:deep` is bound. Measured at 256x256 / 16 spp: deep takes 1.27x the flat time; 1.3 samples/px mean, 16 max; 43.5 B/px handed out. Problems for M257 - Cycles Deep Output: the kernel reserves 2 KB/px (64 fixed slots, about 4.2 GB at 1080p); the absolute merge threshold over-splits curved surfaces; the flatten differs from beauty by up to 0.23; the AOV's flattened pixels came back empty; there are no ids.
- 2026-11-16 — P2.T2 done (`spike/m255/REPORT-P2T2-moonray-deep-arras.md`, 852f31da2). The Arras progressive path fills DeepBuffer exactly as batch does, and flat AOVs survive alongside a deep output, so (d) is viable: about 31 MiB per 1080p frame, 1.8 MiB per 64-row band. However, MoonRay's deep records only the first surface each camera ray hits (`PathIntegrator.cc:1408-1426`), so flattened deep differs from beauty (RMSE 0.106) wherever surfaces overlap. Whether MoonRay deep is still worth wiring up is put to the user.
- 2026-11-16 — **MoonRay first-surface deep (user):** "why have deep with that limitation". Dig deeper to confirm it across transparency modes and deep settings; if it holds, spike a MoonRay fork patch that records every surface along the camera path. M258 - MoonRay Deep Output waits on the result.
