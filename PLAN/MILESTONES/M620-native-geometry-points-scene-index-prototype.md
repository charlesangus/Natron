# M620 - Native Geometry Points Scene Index Prototype

Stage 3 of the M617 plan (`PLAN/DESIGN/2026-11-26-native-geometry-viewer-architecture.md`), conditional: only if M618/M619's heavy-geometry timings show GeoLayers::refresh plus USD change processing above roughly 40% of a drag step. An env-gated filtering scene index that, when only P changed, overrides `primvars/points` and skips the Sdf round trip (option 3, the approach Blender takes for interactive editing). Compare step-to-screen latency at p95 on a modest machine; a clear win leads to a full milestone, otherwise option 1 stands.

## Phase 620.1: Prototype

- [ ] M620.P1.T1 — Points-only scene index behind an env var, and the latency comparison
  - verify: p95 step-to-screen latency, with and without, reported to the user
  - size: L

## Decisions
- 2026-11-26 — Cancelled (user): replaced by F41 - Native Geometry Scene Index (M622-M626), going straight to a native Hydra scene index instead of measure, fix, then prototype. Small viewer bugs moved to M627 - Native Geometry Viewer Cook Fixes.
