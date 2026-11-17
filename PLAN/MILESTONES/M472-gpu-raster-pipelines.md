# M472 - GPU Raster Pipelines

Extend the compute-only GPU layer with graphics pipelines: Slang vertex and fragment entry points in the kernel registry, dynamic rendering into float colour attachments with additive blending, indexed and indirect multi-draw, and a copy into the layer's buffer images — all through `GpuRecordContext`, so node code still never sees Vulkan handles. LensFlare's ghost rasterisation (M467 - LensFlare GPU Ghosts) is the first user; Roto and paint rasterisation (M299 - Roto Render Performance), overlays off the render path (E23 - Overlay Flow) and card splats can follow.

## Scope

**In:**
- Graphics kernels in the registry: a `slang_add_raster` CMake helper compiling a vertex and a fragment entry to SPIR-V with reflection, as `slang_add_kernel` does for compute.
- Pipeline creation and caching in `GpuPipelineCache` (vertex input from storage buffers via vertex pulling, no fixed vertex formats; blend state additive or replace; RGBA32F or R32F attachments).
- `GpuRecordContext::draw(...)`/`drawIndexedIndirect(...)` with Vulkan 1.3 dynamic rendering into a transient `GpuTexture` attachment, then a copy into a buffer image the rest of the layer uses.
- A device capability `rasterFloatBlend` (colour-attachment blend on RGBA32F); raster users fall back to their CPU path when it is false.
- Cancel and device-lost through the existing submit path.

**Out:**
- Depth/stencil attachments, MSAA, geometry and tessellation stages: none until a user needs them.
- Using it in LensFlare: M467 - LensFlare GPU Ghosts.

**Edge cases:**
- Device without RGBA32F blending: `rasterFloatBlend` false; callers use their CPU path, logged once (user, 2026-11-16).
- Huge draws: indirect counts come from a buffer written by a compute pass; the record context bounds-checks against the buffer size.
- Fragments for one pixel blend in primitive order (Vulkan rasterization order), so results are deterministic on one device; across devices and against the CPU they agree within tolerance only.

**Assumptions:**
- Unconfirmed: every target device (NVIDIA, AMD RADV, Intel ANV, llvmpipe) exposes Vulkan 1.3 dynamic rendering and RGBA32F blend; P1.T1 logs what each CI device reports.

## Phase 472.1: Pipelines

- [ ] M472.P1.T1 — Raster capability and attachments
  - files: `Engine/Gpu/GpuDevice.{h,cpp}`, `Engine/Gpu/GpuTexture.{h,cpp}`, `Tests/Gpu/` (new raster capability test, following the folder's existing tests)
  - approach: Enable `dynamicRendering` at device creation when present; query `VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT` for R32G32B32A32_SFLOAT and R32_SFLOAT and expose `rasterFloatBlend`. Let `GpuTexture` be created as a colour attachment with transfer-src usage.
  - verify: gtest: on RADV and llvmpipe the capability is reported and logged; a cleared RGBA32F attachment copies back to a buffer with the clear value.
  - size: M

- [ ] M472.P1.T2 — Raster kernels in the registry and pipeline cache
  - files: `cmake/` (the Slang kernel helper that defines `slang_add_kernel`), `Engine/Gpu/GpuKernelRegistry.{h,cpp}`, `Engine/Gpu/GpuPipelineCache.{h,cpp}`, `Engine/Gpu/GpuShaderCompiler.{h,cpp}`, `Engine/Gpu/Kernels/raster_test.slang` (new), `Engine/Gpu/Kernels/raster_test.cmake` (new)
  - approach: `slang_add_raster(name file VERTEX entry FRAGMENT entry REGISTRY NatronGpu)` emits both stages with reflection into a `GpuRasterId`. The cache builds graphics pipelines keyed by (raster id, attachment format, blend mode) with dynamic viewport and scissor; vertex pulling from storage buffers only.
  - verify: the build registers `raster_test`; a gtest creates its pipeline for RGBA32F additive and replace on RADV.
  - size: L

- [ ] M472.P1.T3 — Draws in GpuRecordContext
  - files: `Engine/Gpu/GpuRecord.{h,cpp}`, `Tests/Gpu/` (new raster draw test)
  - approach: `beginRaster(attachment, clear)`, `draw(rasterId, bindings, push, vertexCount, instanceCount)`, `drawIndexedIndirect(rasterId, bindings, push, indexBuffer, indirectBuffer, drawCount)`, `endRaster()` then `copyToImage(buffer)`. Barriers between compute writes of vertex or indirect buffers and the draw, and between the attachment and the copy. Abort checks between draws as between dispatches.
  - verify: gtest: two overlapping triangles with additive blend give 2.0 in the overlap and 1.0 elsewhere, edge pixels following the top-left rule; an indirect draw whose count a compute pass wrote draws that many triangles; an abort between draws returns promptly.
  - size: L

**Verification gate:** build and ctest green, the raster tests passing on RADV and llvmpipe; the GPU bench and existing GPU parity suites unchanged; capability results per CI device recorded in Decisions.

## Decisions

- 2026-11-16 — **A graphics pipeline, not a compute rasteriser, for LensFlare (user, after a detailed pros/cons comparison):** hardware raster is fastest for many lights and 4K and is reusable for Roto, paint and overlays; parity against the CPU is within tolerance, not bit-exact. Ships on its own, outside F31 - Lens Flares.
- 2026-11-16 — **Without RGBA32F blending, raster users fall back to their CPU path (user).**
