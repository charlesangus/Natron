# M67 covers all four core-node families

2026-10-06, user decision. M67 rewrites as native nodes, in this order: colour (Grade, ColorCorrect, Saturation, Clamp, Invert, Add/Multiply/Gamma), merge + generators (Merge, Dissolve, Constant, CheckerBoard), spatial (Blur, Transform, Crop, Reformat, Position) and keying/misc (Keyer, ChromaKeyer, Erode/Dilate, EdgeDetect, ColorLookup). Answers M67's "which nodes are core" blocker. Execution order: M64 first, then M67, both stacked on M63's PR for one parcel UAT. Headless GL (former M63.P5.T4) is its own deferred milestone M68.
