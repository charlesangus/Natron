# M260 - Cache Invalidation Follow-ups

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Filed from M242 - Cache Invalidation Audit (2026-10-27). The full list, with file:line evidence, is the follow-up section of `docs/cache-invalidation.md` on M242's branch:
- **N3:** the OIIO header and Raw decode caches pick up a changed file while the node hash doesn't move, so old and new tiles mix until Reload. Needs the user's call: pin the reader to the file's stamp, or have the node notice the change and move its hash.
- **N1, N2 (USD):** an authored layer is stored under a key taken before authoring; sublayers changed while the app was closed aren't in the hash until the first Reload.
- **T3, T4, T5, T8 (topology):** T8: a render starting between an input write and the following computeHash() sees the new inputs under the old hash (T7, the check-then-apply race, was fixed in M242).
- **(was T3, T4, T5, T7):** the inspector marks its hash before changing the input; input-change work runs under the old hash; a direct input edit can race a render that starts just after the "is it rendering" check.
- **V2, V3 (timeline line):** any one tile marks a frame cached whatever its view, layer or draft flag; the line keeps old frames after a failed, black or paused render.
- **G1, G2 (GPU):** the host import cache can outlive buffers it never saw freed (small mallocs, mmapped disk-cache images); the shader reject list ignores the device.
- **I3, I4, E2 (string replace on rename), E3, A4, A5, K3–K8:** lower-risk keying and expression issues.

Each fix enables or adds a CacheStress case.

## Decisions

- 2026-10-27 — **Reload is the recheck (user):** Natron never watches files on disk. For N3, a Read pins to the file as it was when last loaded or reloaded: the OIIO header and Raw decode caches must not pick up a changed file on their own; only Reload re-reads it (and moves the node hash). Old header plus new pixels must never mix.
