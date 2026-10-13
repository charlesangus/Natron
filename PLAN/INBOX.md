## Pending

### 2026-10-13T00:00:00Z — change-request
- refs: none
- Pre-existing bug found during the M56 end build: at `QT_SCALE_FACTOR=2`, clicking directly on a curve in the curve editor doesn't select it (a click aimed at y=150 only picked `gamma.r` at y=14), so the curve hit-test likely ignores the device pixel ratio. At 1× it works. Needs a home: a task in a curve-editor milestone (M96?) or a new small fix milestone.
