# Milestone 67: Rewrite core nodes as native nodes

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Natron's everyday nodes (Grade, Merge, Blur, Transform, ColorCorrect, Constant, CheckerBoard, Shuffle-adjacent colour tools, …) are OpenFX plugins from openfx-misc hosted through the OFX ABI. M62.P5.T1 measured that 75–80% of the ~0.65 MB per idle node is OpenFX-specific (property-set copies and OFX→knob glue), and the HD profiles show plugin pixel code and host/plugin copying dominating per-node time. The native-node framework from M17 (`Engine/Nodes/NativeEffectBase`) already hosts the deep and channel nodes with typed edges, direct knob declaration and no ABI boundary. This milestone rewrites the core 2D node set as native nodes, one family at a time, keeping script names, knob names and defaults compatible with the OFX versions so projects and Python scripts keep working, and retires the corresponding openfx-misc plugins from the default bundle.

Blocked on: M62's gate (its bench harness and `compare.py` are the measurement), the user's list of which nodes count as "core" and in what order, and a first measurement that justifies the rewrite: a native-node chain benchmark (per-node build time, frame time and RSS versus the OFX Grade chain) — this is the first task of the milestone, moved here from M62.P6.T1 by user decision on 2026-10-03.

Acceptance sketch:
- A tiny chain of 1000 native core nodes builds and renders with a measured per-node memory and time cost, recorded in `tools/bench/BASELINE.md` next to the OFX numbers.
- Each rewritten node is a drop-in: same plugin ID or a registered alias, same knob script names and defaults, projects saved with the OFX version load onto the native one (no backward compat for the fork's own earlier formats, per the clean-break rule, but OFX→native within one version is a migration the user needs).
- The default plugin bundle no longer ships the replaced openfx-misc plugins; the OFX plugin integration tests are adjusted.
- Pixel output of each native node matches its OFX counterpart within a documented tolerance on the existing render tests.
