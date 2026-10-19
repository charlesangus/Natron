# M137 - Native Base Class Flexibility

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Investigate whether `NativeImageEffect` (`Engine/Nodes/Image/NativeImageEffect.h`) needs mixins, policies or another composition mechanism, so native nodes can take only the behaviour they need instead of one large base class special-casing them through runtime traits. The user's concern is that a big base class with per-feature flags is fragile. The motivating example is "(Un)premult by": Blur, EdgeDetect and ErodeDilate expose it only because `traits.hostUnPremult` is on, and every part of the base (render, GPU eligibility, identity rules) checks the flag. Filters must always work on premultiplied images, so they should not have it. The mask/mix stage, generators that carry neither, and anything else the base bundles are open questions for the same investigation. The output is a design the user approves, then the extraction it calls for.

Blocked on: a grill session with the user to settle the scope and the design (which responsibilities are separable, the composition seam, how the GPU path and identity rules hook in). M121 - Native Routing And Mix and M122 - Native Colour Utilities both assume the base provides "(Un)premult by", so they wait for this.

Acceptance sketch:
- A design note the user approves, covering the base class's responsibilities and the chosen mechanism.
- Blur, EdgeDetect and ErodeDilate no longer show "(Un)premult by" and filter premultiplied data as stored.
- The colour nodes keep the selector, with the same behaviour on CPU and GPU.
- No per-feature trait check remains inside the shared base's render, GPU and identity paths for the stages the design extracts.

## Scope

**In:**
- A spike first: `NodeSpec`, `StagePlan` and both executors, with Grade (Point), Blur (Passes) and a new Switch (Route) on CPU and GPU. Then a design note, written from what the spike found, as a decision file. Then a stop for the user's approval of both. The spike's code lands on the milestone branch and is kept if the design is approved, or reverted if it is not.
- A kind-agnostic shared layer in `NativeEffectBase`, which deep, scene and geometry nodes adopt later instead of forking:
  - `NodeSpec` replaces `NativePluginDescription`. It holds identity, grouping and RFV reads, plus inputs with kind, role (Source, Aux, Mask, Variadic), optional, time, boundary and layer policy.
  - One route/identity hook, returning `{None | Input(i, t) | Black}`. The scheduler and GPU placement read it.
  - A common `KnobReader` and `bake()` into a trivially-copyable `Params`. Knobs are read once per evaluation.
- The image framework:
  - An `ImagePipelineSpec` with stages (Premult, MaskMix, PassThroughUnprocessed), a layer policy (Color with defaults, All, TargetSelect, None) and an op kind (Point, Composite, Route, Passes, Resample, Custom).
  - The `Op` concept, one `StagePlan`, and the two executors `runStagesCpu` and `recordStagesGpu`.
  - Identity is composed and never overridden.
  - GPU feasibility is a property of the plan, which placement reads instead of re-reading knobs by name (`GpuPlacement.cpp:518,539`).
- Every native node moves onto `NodeSpec`, including Shuffle, AddLayers, RemoveLayers, the metadata nodes, NativeRead/Write, DeepToImage and MissingPluginNode.
- Every image node moves onto the image framework: the 7 colour point-ops, the generators, Blur, EdgeDetect, ErodeDilate, the Transform family (as Resample), Crop, Reformat, Position, Dissolve, Keyer and ChromaKeyer.
- Switch, a new native Route node, built here as the Route proof. Its brief moved from M121 - Native Routing And Mix (M121.P1.T1, cancelled):
  - 64 optional image inputs named "0".."63", modelled on Dissolve. `which` is an animatable int, and its display range follows the highest connected input (`Dissolve.cpp:277-280`). There is no `automatic` knob (user, 2026-10-13).
  - The route hook returns the clamped `which` input at the same time and view. RoD is the chosen input's. It never renders.
  - It forwards `getTransform` from the chosen input if the framework supports concatenation; otherwise concatenation is left out.
  - It registers through today's manifest; M120 - Native Node Port Kit moves it onto a fragment with every other node. Moving the PyPlugs to it and retiring `switchPlugin` stay with M121.
- Blur, EdgeDetect and ErodeDilate lose "(Un)premult by" and filter premultiplied data as stored.
- The framework creates the premult, mask and mix stage knobs and places them together below the line: [separator] → (Un)premult by → mask channel and Invert mask → Mix. That is M95 - Knob Cleanup And Keyer Redesign's agreed footer order without the clamp row, which M95 adds above "(Un)premult by" and extends to OFX nodes.
- Every point and composite GPU body becomes an `IPixelOp`/`IBinaryPixelOp` entry in the `pixelwrap.slang` style. There are no standalone kernels such as grade.slang's `grade_main`.
- Shared C++/Slang maths headers become the parity convention, and the Op names its header.
- `NativeImageEffect`, `NativeImageTraits` and `SpatialFilterSupport` are deleted. Merge is re-based as a Custom node that keeps its own `render()`.
- The gate that proves done has three parts:
  - Every migrated node, wherever behaviour is not meant to change, is bit-exact on the CPU against golden renders captured before the migration. GPU output stays within the README parity tolerances.
  - tools/bench in-graph timings for Grade, Blur, Transform and Dissolve on the release AppImage show no regression beyond noise.
  - Xvfb screenshots of the stage-knob panels are shared with the user before sign-off.

**Out:**
- The clean-sheet Merge rewrite. Merge goes Custom here. Owner: M101 - Clean-Sheet Merge Node.
- Switch's siblings and the rest of routing/mix. Owner: M121 - Native Routing And Mix.
- New GPU ports for EdgeDetect, Reformat, Crop, Position, Dissolve and the Shuffle family, plus multi-plane placement. Owner: M110 - Remaining Native GPU Ports.
- Per-node registration fragments, built on `NodeSpec`. Owner: M120 - Native Node Port Kit.
- Optimising the Passes executor for Blur's in-graph overhead. Owner: M139 - Blur In-Graph Overhead.
- Plain IDs and sibling merges; IDs carry over unchanged here. Owner: M111 - Native Node Identity Cleanup.
- The Keyer trapezoid redesign, clamp black/white placement, and the hidpi/interactive knob cleanup. Owner: M95 - Knob Cleanup And Keyer Redesign.
- Fusing point-op regions. Owner: M87 - Point-Op Kernel Fusion.
- Deep per-sample plan types. Owner: M21 - Deep Tier-2 And Bridges.
- Geometry plan types. Owner: M54 - SOPs-Style Geometry.
- Scene authoring. Owner: M19 - USD/Hydra Foundation.
- Multi-plane outputs, such as Despill's spill map. No owner.
- A declarative knob table that generates `initializeKnobs()`. No owner.
- A single-source CPU/GPU kernel JIT. No owner.

**Edge cases:**
- Switch with `which` beyond the last connected input clamps to the highest connected input. With no inputs connected it outputs black and an empty RoD.
- Switch with an animated `which` re-routes per frame with no cache poisoning, because the routed input is part of the frame's hash.
- A filter at default settings stays bit-exact when "(Un)premult by" goes away, because the selector defaults to None (`Node.cpp:2909`). Only renders that set it change, and their GPU parity cases are removed with the knob.
- Merge stays on its own `render()` as a Custom node until M101 - Clean-Sheet Merge Node, so no second framework survives this milestone.
- The scene-to-image bridges (HydraRender, ProjectTexture) remain Custom image nodes with a scene input.
- An invalid spec, such as a Premult stage with no source input, a Route op with stages, or a mask role on a non-image input, is a compile error (constexpr spec plus `static_assert`/concepts). There is no runtime path for it.
- GPU refusal falls back to the CPU, and a runtime shader that does not compile is reported once and never rebuilt, as `NativeImageEffect` does today.
- Knob script names (`mix`, `maskInvert`, per-node knobs) are unchanged, so content hashes and the reordering at `Node.cpp:3303` keep working.

**Assumptions:**
- Unconfirmed: Switch keeps 64 fixed optional inputs like Dissolve, rather than growing inputs dynamically.
- Nothing in the content hasher (M100 - Content Node Hashing) hashes class-level traits, so replacing `NativeImageTraits` with a spec leaves hashes unchanged. The Fable design pass did not verify this.
- `canRenderOnGpu` and the plan stay deterministic between placement and record time (`GpuRegionTask.cpp:587`).
- Unconfirmed: GPU-side changes compile and run only on the remote build until a local slangc tree exists. The spike's GPU half is verified there.
- Unconfirmed: the Despill spill-map mode in M122 - Native Colour Utilities is refused for the GPU, or stays Custom, until multi-plane outputs exist.

## Decisions

- 2026-10-19 — Scope is a blue-sky redesign, not a trait fix. The Fable design has a declarative spec, an `Op` concept, and one `StagePlan` with CPU and GPU executors (user): mixins and per-feature flags leave `render()` overridable, which is where the triplicated blend and identity rules came from.
- 2026-10-19 — The milestone covers every image op kind (Point, Composite, Route, Passes, Resample, Custom) and migrates every image node (user): one milestone rather than a split, accepting that it serialises the E3 ports while it runs.
- 2026-10-19 — Build the kind-agnostic shared layer (`NodeSpec`, the route hook, `KnobReader`/`bake`) now (user): the image framework proves it, so deep, scene and geometry adopt it rather than fork it.
- 2026-10-19 — Every native node moves onto `NodeSpec`, non-image ones included (user): no second way to declare a node survives.
- 2026-10-19 — This milestone runs before M120 - Native Node Port Kit, whose fragments are generated from `NodeSpec` (user): registration is designed once. M120 gains a dep on M137.
- 2026-10-19 — Merge is not migrated. It goes Custom and M101 - Clean-Sheet Merge Node builds on the framework (user): migrating a node that is about to be rewritten is wasted work. M101 gains a dep on M137.
- 2026-10-19 — Delete `NativeImageEffect` at the end of this milestone (user): no second framework survives.
- 2026-10-19 — This milestone runs before M110 - Remaining Native GPU Ports (user): those ports become GPU bodies for `recordStagesGpu`, so nothing is hand-wrapped and then migrated. M110 gains a dep on M137.
- 2026-10-19 — This milestone owns stage-knob placement below the line, taken from M95 - Knob Cleanup And Keyer Redesign (user): the framework creates those knobs, so it places them.
- 2026-10-19 — Order of work is spike, then design note, then one approval stop (user). The spike covers Grade, Blur and Switch on CPU and GPU, and its code is kept if approved.
- 2026-10-19 — Switch moves from M121 - Native Routing And Mix into this milestone as the Route proof (user). No native Route node exists yet.
- 2026-10-19 — Gate: bit-exact on the CPU, README tolerance on the GPU (user): the CPU maths is unchanged, so any ulp change is a bug.
- 2026-10-19 — Gate: no performance regression beyond noise (user): the framework must not tax every node.
- 2026-10-19 — M139 - Blur In-Graph Overhead runs after this milestone (user): it then optimises the shared Passes executor for every node that uses it. M139 gains a dep on M137.
- 2026-10-19 — IDs carry over unchanged; renames stay with M111 - Native Node Identity Cleanup (user): this milestone is behaviour-preserving.
- 2026-10-19 — An invalid spec is caught at compile time (user).
- 2026-10-19 — Keyer and ChromaKeyer are migrated as they are. M95 - Knob Cleanup And Keyer Redesign's redesign lands on the framework (user). M95 gains a dep on M137.
- 2026-10-19 — Point and composite GPU bodies are converted to `IPixelOp`/`IBinaryPixelOp` here (user): a standalone kernel would be a second GPU path, and it is also the precondition for M87 - Point-Op Kernel Fusion.
