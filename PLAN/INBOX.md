## Pending

### 2026-10-16T09:00:00Z — change-request
- refs: none
- New milestone requested (user accepted the miss in M90 - Render Path Serial Overhead): **Blur In-Graph Overhead** (E6). In-graph native Blur at UHD on 16 threads is 3.1× its bare kernel at FIR σ3 (4.70 vs 1.51 ms/Mpx) and 2.0× for IIR (6.83 vs 3.37). Profile on the 16-thread host (symbolised build) and cut the fixed per-task overhead (fill, `writeWindow` finish, planar buffer allocation) and the padded-region cost at large σ. Data: `M90-render-path-serial-overhead.md` Decisions 2026-10-16. Deps: M90.

