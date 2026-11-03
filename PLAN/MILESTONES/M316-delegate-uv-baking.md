# M316 - Delegate UV Baking

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Bake render-delegate renders, such as Cycles, onto UVs through the bake engine's pluggable backend, so USDBakeTextures (and TexturePaint's Bake Inputs) can capture lit, path-traced looks (user, 2026-11-02 /cat-grill).

Blocked on: M313 - USDBakeTextures Node's backend interface, and M251 - Cycles Rendering for a delegate to bake with.

Acceptance sketch:
- USDBakeTextures offers a delegate backend; choosing Cycles bakes its render onto the mesh's UVs.
- Output matches the built-in backend's UDIM layout.
