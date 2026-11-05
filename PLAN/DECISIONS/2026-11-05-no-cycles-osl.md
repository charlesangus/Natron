# 2026-11-05 — No OSL in the bundled Cycles

Cycles stays built with `WITH_CYCLES_OSL=OFF` (user). OSL is a session-wide switch in Cycles and its GPU route is OptiX only, so nothing for AMD. Native Cycles shader support therefore excludes the OSL script node, and MaterialX through generated OSL isn't available unless this is reopened. MaterialX is set aside for now (user, same day).
