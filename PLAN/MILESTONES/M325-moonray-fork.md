# M325 - MoonRay Fork

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Fork the MoonRay sources Natron builds (openmoonray and the submodules we patch: hydra-moonray, mcrt_dataio, scene_rdl2/arras as needed) under charlesangus, as was done for Cycles (charlesangus/cycles natron/deep), and move every patch `tools/ci/local/build-moonray.sh` applies with sed (IsSupported override, HostName debug print, depth channel name, session-definition log levels) into commits on a `natron/...` branch. The build script then pins the fork by commit and checksum and applies no patches (user, 2026-11-04: "fork moonray, similar to cycles, don't patch it in this repo").

Blocked on: M252 - MoonRay Rendering landing, so the patch set to carry is final.

Acceptance sketch:
- charlesangus forks with a `natron/` branch holding each patch as a commit with its reason.
- build-moonray.sh has no source patches; it pins the fork's commit and records it in a SOURCE file, like build-cycles.sh.
- The dev image rebuilds and the MoonRay tests pass unchanged.
- The fork's hdMoonray overrides `GetMaterialRenderContexts` to return `moonray`, so it reads `outputs:moonray:surface` and falls back to universal outputs (needed by M344 - Camera Projection In Delegates; user asked for native shader support, 2026-11-05).
