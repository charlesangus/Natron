## Pending

### 2026-11-27 — change-request — AdjustBBox Num Pixels 0 regression on main
- from: bosley-executor (cat-pm)
- refs: M351
- body: `Tests/gui/m121_routing_mix.py` fails one check on main 6befcdadb7 (release, run against main's AppImage): "[adjustbbox] Num Pixels 0 leaves the region as it was, so the centre is empty again ((255, 0, 0))" — setting AdjustBBox back to 0 pixels no longer restores the region. Seen on M351 - Shader Graph Presentation gates 18-20 too. Proposed small fix milestone: bisect main for the change (likely RoD caching or a recent Merge/blur-family change), fix, and run m121 in PR CI's GUI set so it can't regress unseen.
