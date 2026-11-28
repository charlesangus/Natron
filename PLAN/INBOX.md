## Pending


### 2026-11-27 — change-request — Cycles VDB field changes may not refresh a live volume
- from: bosley-executor (cat-pm)
- refs: M554
- body: In the Cycles fork, `src/hydra/field.cpp` (~:140) marks a single rprim dirty during a scene-index sync, which Hydra rejects ("requires emulation"). A VDB field that changes on a live engine probably won't refresh its volume. M554 - Scene Camera For Delegate Renders fixed the same pattern in mesh.cpp by calling `MarkAllRprimsDirty(DirtyTopology)`. Apply the same fix to field.cpp and add a live-edit volume refresh test (F1 - VDB Volumes area).

### 2026-11-27 — change-request — F33 template update workflow redesign (user UAT)
- from: bosley-executor (cat-pm), user UAT of F33 - Graph Diff And Merge round 1
- refs: F33, M103, M104, M512
- body: User UAT found design issues to discuss in a side session before testing continues (do not implement yet). Full notes: `.local/UAT_Feedback.md` "F33 - Graph Diff And Merge (UAT round 1, 2026-11-27)".
  1. **Update from template** only re-reads the originally imported file. Intended: supe publishes template vN, artist imports it, both work, supe publishes vN+1, artist updates from vN+1 with a three-way merge (base = vN as imported, theirs = vN+1, mine = artist's graph). Needs a way to pick the new version (and remember the base that was imported).
  2. Picking v002 through the file picker (after moving the template away) added every node again instead of three-way merging — the template identity / base tracking doesn't survive a different file.
  3. **Compare with base:** ghosts are drawn at their absolute base-file positions instead of relative to the surrounding nodes.
  4. **Node moves count as changes:** moving the whole template shouldn't count; moving a node within the template possibly should — needs a rule.
  5. **Probable bug:** importing a second template, changing knobs, then Update from template appears to do nothing.
