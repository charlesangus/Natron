# M139 - Blur In-Graph Overhead

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Cut what native Blur costs in a graph beyond its bare kernel. In-graph Blur at UHD on 16 threads is 3.1× its bare kernel at FIR σ3 (4.70 vs 1.51 ms/Mpx) and 2.0× with IIR (6.83 vs 3.37). The suspects are fixed per-task overhead (the fill, the `writeWindow` finish, planar buffer allocation) and the padded-region cost at large σ. The user accepted this miss in M90 - Render Path Serial Overhead. The measurements are in `M90-render-path-serial-overhead.md`, under Decisions dated 2026-10-16.

Deps: M90 - Render Path Serial Overhead, M137 - Native Base Class Flexibility.

After M137 - Native Base Class Flexibility, Blur is a Passes op, and the fill, finish and buffer allocation live in the shared executor. This milestone optimises that executor, so every Passes node gains (user, M137 grill, 2026-10-19). Blur output stays bit-identical.

Blocked on: M137 - Native Base Class Flexibility merging, and a symbolised profile of in-graph Blur on the 16-thread host. Which overhead dominates decides the tasks, so they can't be written before it.

Acceptance sketch:
- In-graph Blur at UHD on 16 threads comes measurably closer to its bare kernel at FIR σ3 and at IIR. The target ratio gets set once the profile is in.
- The profile and the before and after numbers are recorded in this milestone's Decisions.
- Blur output stays bit-identical.
