## Pending



### 2026-11-27 — change-request — Keep Both Keys is impossible for same-frame keys (user decision)
- from: bosley-executor (cat-pm), user answer to the M104 open question
- refs: M104, F33
- body: **User decision:** when both sides keyed the same knob at the same frame, "Keep Both Keys" makes no sense — two keys can't share a frame — so the option must not be offered there (only Mine / Theirs). M104 - Merge Resolution UI currently moves theirs one frame after mine (or halfway to the next key). Remove that offset rule and the Keep Both choice for same-frame key conflicts; Keep Both stays only where keys are at different frames. Fold into an F33 - Graph Diff And Merge member (M646 or M647) or a small new member, since M104 is done.

### 2026-11-27 — change-request — Roto render: still-frame and edit speedups (M299 follow-up)
- from: bosley-executor (cat-pm)
- refs: M299
- body: M299 - Roto Render Performance shipped below two of its targets (user accepted): 100-shape still UHD later frames 2.2-2.4x vs the 3x target, and knob edits (edit-colour, edit-grade) 1.2-1.4x vs 1.5x. Proposed milestone "Roto Still Frame And Edit Speed": profile the 100-shape still UHD later frame and the edit cases on M299's build (Merge compositing and RGBA conversion were the biggest costs in the M299 profile), cut the next largest cost, re-bench with tools/bench/roto_render_bench.sh on a cloud VM. Also: the roto GUI stroke bench (`tools/bench/gui/run.sh --host --driver roto --ops stroke_5px,stroke_50px,stroke_300px,pen_up`) ran 40+ min with no records on an RTX 3060 VM for both main and M299 — make it finish within the bench budget (likely the 300-node upstream comp build) so stroke latency can be gated.
