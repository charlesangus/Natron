# M492 - Image-Driven Warp Providers

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

IDistort, STMap and IWarp become providers in M153 - Distortion Concatenation's chain, so they can fold into a downstream warp, not just consume one (M153 Decisions, 2026-11-17). Their inverse map comes from a UV image, which is known only after it renders. The provider uses M179 - Render Graph Continuation's probe → extend → continue sequence. Its UV image renders over the window pulled back from downstream, as a probe. The frame is then extended to request only the source footprint those UVs point at. Then the continuation samples the source once, through the composed map.

Blocked on: M153 - Distortion Concatenation and M179 - Render Graph Continuation landing, since both are in progress and the chain-step and continuation APIs this extends are still being built.

Acceptance sketch:
- STMap → Transform and Transform → IDistort → Transform each resample the source once, and match the unchained result in the interior.
- The source request covers only the UV footprint (with the filter radius), not the full RoD.
- A chain of two image-driven providers composes with one probe per provider, inside M179's depth limit.

## Scope

**Out:**
- LensCorrect per-channel coordinates: M153's follow-up.

**Edge cases:**
- UVs containing NaN or pointing outside the source: "no source" NaN travels as in M153, and the read is black.
- A UV footprint covering the whole source: the request is the whole source, with no special casing.
- Provider declines (mask, mix ≠ 1, STMap output mode): the chain ends before it, as M153's providers do.

**Assumptions:**
- Unconfirmed: the probe's UV render is cached and reused by the continuation, not rendered twice.
