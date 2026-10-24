# M166 - Non-Float Plane Model

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Decide how image data that isn't float32 is modelled, and file its implementation. Today nodes disagree. Shuffle and ShuffleCopy drop any plane that is not float (`Engine/Nodes/Channel/Shuffle.cpp:875-877`), so it reads as 0. AddLayers and RemoveLayers convert byte and short planes through ChannelCopy's depth table (`Engine/Nodes/Channel/ChannelCopy.cpp:86-114`), which they can do because `NativeEffectBase::addSupportedBitDepth` declares byte, short and float (`Engine/Nodes/NativeEffectBase.cpp:512-517`). The staged pipeline is float-only (`Engine/Nodes/Image/StagedImageEffect.cpp:63-66`), and GPU regions convert their inputs to float (`Engine/GpuRegionTask.cpp:826-830`). The user suggests non-float data may be a different *kind*, as deep is beside image, rather than a per-node depth policy, and also wants Read to expose an EXR's non-float layers at their native types, which points towards a bit depth per layer (an EXR with a uint32 ID layer and a half beauty layer, both accessible). Raised by the user during M110 - Remaining Native GPU Ports, which keeps each node's current behaviour meanwhile. Filed 2026-10-24.

Blocked on: a design discussion with the user (`/cat-discuss`) on the model itself: per-layer depth, a separate kind, a per-node policy, or a mix. The user asked to rethink the framing before any survey or tasks are written (grill, 2026-10-24). Draft survey tasks from the first elaboration pass (bit-depth negotiation path, sources, consumers per `addSupportedBitDepth` override) are a starting point once the framing is settled.

Acceptance sketch:
- A design note on the plan branch (`PLAN/DESIGN/<date>-non-float-planes.md`) that surveys where non-float data arises and how each consumer treats it, sets out the options, and records the user's decision.
- Native Read's EXR handling of half and uint32 channels is part of the survey and the chosen model.
- The implementation is requested as follow-up stub milestones through the inbox, including one that brings Shuffle, ShuffleCopy, AddLayers and RemoveLayers to one consistent behaviour.

## Scope

**In:**
- Half float, as well as byte, short and integer types.
- Native Read exposing an EXR's non-float layers at their native types.

**Out:**
- Implementing the model: the follow-up milestones.
- Any quick consistency fix to the channel nodes: M110 - Remaining Native GPU Ports keeps current behaviour meanwhile.

**Edge cases:**
- Integer-valued planes (object IDs, cryptomatte-style data) must stay exact end to end. An option that rounds them is disqualified.
- Mixed depths across the layers of one input.

**Assumptions:**
- The DataKind framework (`Global/Enums.h:515-519`, `Engine/Nodes/NodeSpec.h:98`; M17 - Typed Edges Framework, M159 - Kind-Agnostic Groups) is the reference for the "separate kind" option.

## Decisions

- 2026-10-24 — **Half is in scope:** it is real storage today and halves cache and GPU memory, which matters on modest machines (user, grill).
- 2026-10-24 — **Integer data must stay exact:** IDs and cryptomatte-style planes are a hard requirement on any model (user, grill).
- 2026-10-24 — **Independent of M167 - Per-Frame Node Metadata:** the two run in either order, and M166's follow-ups adapt to however M167 carries bit depth (user, grill).
- 2026-10-24 — **Read should emit non-float EXR layers natively, and per-layer depth is on the table:** the user's answer reframed the investigation, and asked for a bigger rethink before planning, so the milestone stays a stub until that discussion (user, grill).
