# Native shader networks in a builder node

Rich materials use each renderer's own shading network, with full or nearly full coverage of what the renderer offers, rather than a translation. MaterialX is set aside for now (user, 2026-11-05).

Shader networks don't fit Natron's node model: shaders have up to about 170 typed attributes, while Natron nodes have a few arrows positioned by angle (`Gui/NodeGui.cpp:1941`). So the design is VOP-like (user). One `USDMaterialBuilder` appears in the main graph as an ordinary node, and diving in shows a typed, left-to-right shader graph. Shaders are real Natron nodes in a Group, with a new drawing, subject to M349 - Shader Node Spike. Most attributes are knobs, and key or promoted ones are ports. One builder holds networks for several renderers on one `UsdShadeMaterial`.

Delivered as F2 - Native Shader Networks. Details and alternatives are in its member milestones' `## Decisions`.
