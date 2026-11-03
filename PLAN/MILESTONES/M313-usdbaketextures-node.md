# M313 - USDBakeTextures Node

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A standalone Write-like node whose main job is baking a scene's bound look, per channel, into UDIM texture files. It shares M53 - Projected Texture Painting's MaterialBake engine and its pluggable backend. It also finishes M234 - Camera Projection Nodes' USDWrite bake for meshes that aren't card-like (user, 2026-11-02 /cat-grill).

Blocked on: M53 - Projected Texture Painting's TexturePaint core, which owns the MaterialBake engine and its backend interface; and M234 - Camera Projection Nodes' USDWrite bake path.

Acceptance sketch:
- Rendering USDBakeTextures writes one UDIM file set per chosen channel of the bound material.
- M234's USDWrite bake produces correct textures on non-card meshes through the same engine.
- The bake runs async and never blocks the GUI.
