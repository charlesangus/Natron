## Pending

### 2026-10-04T23:15:35-04:00 — answer
- refs: M61, M65, M66, M37, M50, M60, M62
- Parcel UAT result from the user: "These all look good." The user walked through the UAT for the stacked PRs #34 (M61), #35 (M65), #36 (M66), #37 (M37), #38 (M50), #39 (M60) and #40 (M62), and says they are OK to merge, bottom-up (#34 first). This answers the parcel-UAT items under `# Open questions` for all seven milestones (M61 sign-off, M65 P8.T3, M66 P6.T3, M37 P4.T3/P6.T2, M50 P6.T2, M60 P5.T1/P6.T2, M62 scale check). Check off the UAT-gated tasks, move the `blocked` rows to `done` once each PR merges, and remove the answered questions. The fork PRs (openfx-misc#5/#6, openfx-io#8/#9/#10, openfx-arena#2/#3/#4, openfx-natron#3) merge with their milestones, followed by re-pinning `tools/ci/local/fetch-assets.sh` and `libs/OpenFX`. After #40, re-run `tools/bench/run_matrix.sh` on `main` to refresh `BASELINE.md`.
