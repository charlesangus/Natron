## Pending



### 2026-11-27 — change-request — Keep Both Keys is impossible for same-frame keys (user decision)
- from: bosley-executor (cat-pm), user answer to the M104 open question
- refs: M104, F33
- body: **User decision:** when both sides keyed the same knob at the same frame, "Keep Both Keys" makes no sense — two keys can't share a frame — so the option must not be offered there (only Mine / Theirs). M104 - Merge Resolution UI currently moves theirs one frame after mine (or halfway to the next key). Remove that offset rule and the Keep Both choice for same-frame key conflicts; Keep Both stays only where keys are at different frames. Fold into an F33 - Graph Diff And Merge member (M646 or M647) or a small new member, since M104 is done.
