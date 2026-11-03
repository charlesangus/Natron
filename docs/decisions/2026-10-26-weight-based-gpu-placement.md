# Weight-based GPU placement replaces the measured cost model

2026-10-26 — user, during the M184 - Variable-Size Filter Kit GPU seeding run; carried out by M239 - Weight-Based GPU Placement.

GPU placement stops using measured costs. Each node declares a weight class: Point = 1, Filter = y, or Heavy (send my region to the GPU). Runtime-shader nodes are always Heavy, and a node may scale its weight with its knobs. A region goes to the GPU when any member is Heavy or its summed weight reaches z. When transfers are free, any GPU-capable region goes. A pixel floor keeps tiny regions on the CPU. y, z and the floor are constants picked once. The startup transfer calibration stays.

The cost seed rows, the in-session EMA, `render-profile-report.py --emit-cost-table`, `*.costseed.inc` and `Engine/GpuCostSeeds/` are removed with no compatibility layer. No node milestone measures or writes cost seeds from now on, and any such task in an unstarted milestone is void.

## Rationale

- The seeds hold for one machine only: a 16-thread CPU and an RX 7900 XTX. On other hardware the absolute ms are off by integer factors in both directions, and the CPU/GPU ratio that placement depends on is wrong too.
- They can't be reproduced: the inputs live only in `build/bench/` on the dev host, the GPU rows were hand-copied with ad hoc adjustments, and the host changes hardware across reboots.
- The first frame, the workload that matters, relies entirely on them; the EMA only corrects later frames.
- They gated every node milestone (quiet-host CPU profiles, GPU-host runs) with no clear benefit.
- Kernel and transfer time both scale with pixel count, so work per pixel (the summed weight) is what decides whether a region is worth its transfer. A lone point op is roughly break-even at UHD, so z > 1.
