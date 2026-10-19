# glslang as the runtime shader compiler, and its licence

The user confirmed on 2026-10-16, for M126 - Runtime GPU Shaders, that the Vulkan backend compiles runtime GLSL with glslang, statically linked, and reflects descriptor layouts with vendored SPIRV-Reflect.

Some glslang files and all of SPIRV-Reflect are Apache-2.0. Apache-2.0 is compatible with GPLv3 but not GPLv2, so a release binary that links them is effectively distributed under GPL-3.0-or-later. The user accepted that. Future dependencies don't need to avoid Apache-2.0 for GPLv2's sake.

The comparison and measurements are in `PLAN/DESIGN/2026-10-13-runtime-shader-compiler.md`.
