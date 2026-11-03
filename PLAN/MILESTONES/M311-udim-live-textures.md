# M311 - UDIM Live Textures

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Extend M233 - Live Node Textures with a `<UDIM>` token: `natron://` texture URIs, `USDMaterial` image inputs and USDWrite export all accept it, so every UDIM tile of a node-driven texture shows live in the Viewer's 3D view and exports as a tile set. M53 - Projected Texture Painting depends on it (user, 2026-11-02 /cat-grill).

Blocked on: M233 - Live Node Textures landing; its `natron://` URI scheme and `USDMaterial` image inputs were rewritten during review, so the token's shape depends on the merged design.

Acceptance sketch:
- A texture URI or USDMaterial image input with `<UDIM>` resolves one live texture per tile; every tile shows in the 3D view.
- USDWrite exports a `<UDIM>` asset path plus one file per tile.
- A node feeding a UDIM input is asked for each tile it covers, and an edit refreshes only the affected tiles.
