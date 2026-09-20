# Milestone 60: Deep images get layers/channels like flat images

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Scouted 2026-09-19 while designing M38: `DeepImage` already stores a named-channel
map (`Engine/DeepImage.h:199-308` — "R, G, B, A, Z, ZBack, and arbitrary AOVs") and
`DeepRead`/`DeepWrite` pass every EXR channel through by name. The hole is in the
processing nodes and in the model: `DeepFromImage`, `DeepToImage`, `DeepRecolor`
hardcode RGBA (`Engine/Nodes/Deep/DeepFromImage.cpp:107-121`, `DeepToImage.cpp:71-73`,
`DeepRecolor.cpp:150-151`), `DeepExpression` hardcodes `{R,G,B,A,Z,ZBack}`
(`DeepExpression.cpp:42-43,89-91`), and deep channel names are flat strings with no
layer grouping — `diffuse.R/G/B` is never a `diffuse` layer. Extra channels survive
Read→Write but are silently dropped through any deep processing node.

This milestone brings the deep side onto the same layer model as flat images (the
script-level layer registry and the layer/channel knobs M38 introduces): group deep
channels into `ImageLayerDesc` layers, give `DeepFromImage`/`DeepToImage`/`DeepRecolor`
a channel-set knob, and make `DeepExpression` operate on the channels actually present.

Blocked on: M38 — the registry and the three layer/channel knob types must exist first.

Acceptance sketch:
- A deep EXR with `diffuse.R/G/B` alongside RGBAZ round-trips through DeepMerge,
  DeepRecolor and DeepToImage with `diffuse` intact and selectable as a layer.
- `DeepFromImage` converts the layers chosen in its channel-set knob, not just RGBA.
- `DeepExpression` exposes an expression per channel present in its input.
