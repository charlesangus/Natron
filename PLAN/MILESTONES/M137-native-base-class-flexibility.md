# M137 - Native Base Class Flexibility

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Investigate whether `NativeImageEffect` (`Engine/Nodes/Image/NativeImageEffect.h`) needs mixins, policies or another composition mechanism, so native nodes can take only the behaviour they need instead of one large base class special-casing them through runtime traits. The user's concern is that a big base class with per-feature flags is fragile. The motivating example is "(Un)premult by": Blur, EdgeDetect and ErodeDilate expose it only because `traits.hostUnPremult` is on, and every part of the base (render, GPU eligibility, identity rules) checks the flag. Filters must always work on premultiplied images, so they should not have it. The mask/mix stage, generators that carry neither, and anything else the base bundles are open questions for the same investigation. The output is a design the user approves, then the extraction it calls for.

Blocked on: a grill session with the user to settle the scope and the design (which responsibilities are separable, the composition seam, how the GPU path and identity rules hook in). M121 - Native Routing And Mix and M122 - Native Colour Utilities both assume the base provides "(Un)premult by", so they wait for this.

Acceptance sketch:
- A design note the user approves, covering the base class's responsibilities and the chosen mechanism.
- Blur, EdgeDetect and ErodeDilate no longer show "(Un)premult by" and filter premultiplied data as stored.
- The colour nodes keep the selector, with the same behaviour on CPU and GPU.
- No per-feature trait check remains inside the shared base's render, GPU and identity paths for the stages the design extracts.
