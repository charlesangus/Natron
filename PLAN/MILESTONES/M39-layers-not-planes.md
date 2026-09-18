# Milestone 39: Adopt "layer" terminology instead of "planes"

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Replace the app's "planes" terminology with the more standard "layers" (per
common compositing/EXR usage — confirm exact EXR nomenclature during
elaboration) across UI strings, docs, and, where safe, internal naming.

Blocked on: not yet prioritized — captured as backlog (user request
2026-09-18); needs a scoping pass on how far the rename reaches (UI-only
vs. internal APIs/serialization) before elaboration.

Acceptance sketch:
- User-facing UI uses "layer" instead of "plane" consistently.
- Project files and serialization compatibility are unaffected.
