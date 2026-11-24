# Feature-gap survey triage

Decided with the user, 2026-11-24, going item by item through the Solaris / Gaffer / Katana feature-gap survey (`.local/solaris-gaffer-katana-gap-survey-2026-11-23.md`). The adopted items are E27 - Scene Authoring And Lighting, M562–M589.

- **Prim editing shape (user):** USDSetAttribute stays the raw single-attribute node and gains tweak modes. USDEditPrim stays schema-driven and gains relationship knobs. A new USDConfigurePrim edits prim metadata only. USDConfigureLayer (M449) gains explicit live layer-metadata edits.
- **Light Mixer is a node, not a panel (user).**
- **Declined (user):**
  - Shot variables, Spreadsheet and Variable Switch.
  - Graph bookmarks and panel pinning.
  - Python Script scene node, LOD automation.
  - Farm dispatchers, interactive multi-render, Hydra generative procedurals.
- **Kept out (user; do not re-propose):** Katana's lazy scene model and Op API, CEL/VEX selection languages, Gaffer's full Context system, Capsules, OSL nodes, Houdini geometry bridges, Shot Builder and studio asset APIs, a renderer-to-compositor bridge, Katana's 2D nodes. Physics drop layout was revived, as stub M587.
- **Survey stubs wait for approval (user):** every stub from the survey (M582–M589) is blocked on the user's go-ahead before elaboration.
