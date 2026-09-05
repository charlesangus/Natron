# Milestone 16: Project file format redesign (.ntp successor)

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).
> **Deferred by the user (2026-09-05): future work — do not start or elaborate
> without an explicit go-ahead.**

Replace the boost::serialization XML `.ntp` with a bundle format: a
stored-uncompressed zip container holding a YAML graph member (sparse knobs,
explicit named connections, compact curve encoding) plus mmap-able binary blob
members for heavy per-node data (roto strokes, tracker keyframes). Includes a
one-way import bridge for existing 2.x `.ntp`/`.nps` files, modeled on RB-3's
`SerializationCompat`. Full design, rationale, and investigation evidence:
`DECISIONS/2026-09-05-project-format-bundle-design.md`.

Blocked on: explicit user go-ahead (deliberately deferred as future work), plus
two unresolved design calls to settle at elaboration time — blob encoding (flat
typed arrays vs FlatBuffers) and whether to port RB-3's `Serialization/`
library wholesale or use it as reference for a fresh non-intrusive
serialization boundary. Note also that the required serialization boundary
(decoupling archive shape from Engine classes) is a large refactor that likely
warrants its own preceding milestone when this is elaborated.

Acceptance sketch:
- A project saves as a single bundle file: YAML graph member diffable with
  standard text tools, heavy roto/tracker data in binary members, save remains
  atomic (temp + rename).
- Connections serialize explicitly on the consumer node keyed by input name;
  a file with dangling references loads with disconnected inputs and warnings,
  not a failure; node order in the file does not affect the loaded graph.
- Existing 2.x `.ntp` projects (including the `Tests/fixtures/*.ntp` fixtures)
  import correctly through the compat bridge; `.nps` presets have a story.
- A roto/tracker-heavy benchmark project is ~10x+ smaller on disk and
  measurably faster to save/autosave than its `.ntp` equivalent.
