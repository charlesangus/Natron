# M137 - Premult Stage Extraction

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Filters must always work on premultiplied images, but Blur, EdgeDetect and ErodeDilate expose the "(Un)premult by" selector, because `NativeImageEffect` offers it through a runtime trait (`traits.hostUnPremult`) that every part of the base class checks. The user wants the premult step to be a separable, composable stage rather than a flag inside a large base class: a mixin or policy that the colour family composes in and the filters never have. The same question applies to the mask/mix stage. The design is not settled and is for a grill session to decide, not for this stub.

Blocked on: a grill session with the user to settle the design (what the stages are, the composition seam, how the GPU path and identity rules hook in). M121 - Native Routing And Mix and M122 - Native Colour Utilities both assume the base provides "(Un)premult by", so they wait for this.

Acceptance sketch:
- Blur, EdgeDetect and ErodeDilate no longer show "(Un)premult by" and filter premultiplied data as stored.
- The colour nodes keep the selector, with the same behaviour on CPU and GPU.
- No `hostUnPremult` trait check remains inside the shared base's render, GPU and identity paths.
