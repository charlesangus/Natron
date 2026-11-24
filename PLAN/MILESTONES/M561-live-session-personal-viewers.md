# M561 - Live Session Personal Viewers

Viewers stay shared graph nodes, so everyone can see everyone else's viewer. On joining a live session, each user gets their own Viewer node if the shared project doesn't already have one for them, and their default viewer pane shows that personal viewer. Changing input or channels on one's own viewer then changes only that viewer (it still reaches peers as an ordinary edit of that node); Follow mirrors the leader's viewer as M106 implements it (user, 2026-11-24).

Stub — elaborate before claiming. Open points for elaboration: how a viewer is tied to a user across sessions (user identity vs display name), naming, what the host gets, what happens to a personal viewer when its user leaves, and where it is placed in the graph.

## Decisions
- 2026-11-24 (user): "Each user should have their own viewer, but also be able to see other viewers. Viewers should be shared, but each user should get their own viewer created on join if they don't already have one in their shared script, and their default viewer pane populates with their personal viewer."
