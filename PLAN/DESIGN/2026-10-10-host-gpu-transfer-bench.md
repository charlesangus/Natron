# Host<->GPU transfer microbenchmark (M82.P2.T1)

2026-10-10. Question: is moving a UHD frame host<->GPU per node cheap enough to shape the GPU backend choice (Vulkan compute is the lean)? Spike code: `build/gpu-transfer-bench/` (gitignored: `gl_bench.cpp`, `vk_bench.cpp`, `common.h`; raw output `gl_results.txt`, `vk_results.txt`).

## Machine

| Item | Value |
|---|---|
| CPU / RAM | Ryzen 7 7800X3D, 188 GB |
| GPU | RX 7900 XTX (Navi 31), 24 GiB VRAM |
| PCIe | Gen4 x16 (16.0 GT/s, width 16; max == current; read from sysfs, `lspci -vv` LnkSta not readable unprivileged). Theoretical ~31.5 GB/s per direction |
| Vulkan | RADV, Mesa 25.0.7, Vulkan 1.4.305 |
| OpenGL | radeonsi, GL 4.6 compat, Mesa 25.0.7 (headless via EGL device platform, no X) |
| ReBAR | Yes. Heap 1 is 24 GiB and has DEVICE_LOCAL+HOST_VISIBLE+HOST_COHERENT types (flags 0x7); BAR0 is 32 GiB in sysfs |

## Method

- Frame 3840x2160 RGBA: float32 = 132.7 MB, half = 66.4 MB, 8-bit = 33.2 MB.
- 5 warmup + 60 timed iterations (20 for the extra GL/ReBAR variants); wall-clock per stage, each GPU stage ends in `glFinish` / fence wait. Single-threaded `memcpy` between a pageable host `std::vector` and the mapped staging buffer.
- Memcpy and DMA are timed separately; "total" is their sum (no overlap/pipelining, which a real engine could add).
- GB/s = frame bytes / median time. p90 GB/s = same, using the 90th-percentile (slow-tail) time.
- Paths: GL persistent-mapped PBO (`glBufferStorage` MAP_PERSISTENT|COHERENT, `glTexSubImage2D` / `glGetTexImage` into PBO); GL classic orphan+`glMapBuffer`/`Unmap` per frame (closest to Natron's current path); GL direct client pointer; Vulkan host-visible staging buffer + `vkCmdCopyBufferToImage` / `vkCmdCopyImageToBuffer` on the graphics queue (optimal-tiling image); Vulkan buffer<->device-local buffer; Vulkan CPU write straight into device-local host-visible (ReBAR) memory.
- Vulkan upload staging = host-visible coherent (uncached, write-combined); download staging = host-visible cached.

## Results: float32 UHD (132.7 MB), median ms and GB/s

| Path | Dir | CPU memcpy | DMA / GPU copy | Total ms | Total GB/s (p90) |
|---|---|---|---|---|---|
| GL persistent PBO | up | 7.0 ms (19.0) | 0.52 ms (see note 1) | 7.5 | 17.7 (17.0) |
| GL persistent PBO | down | 19.4 ms (6.8) | 5.1 ms (26.0) | 24.6 | 5.4 (5.4) |
| GL map/unmap PBO | up | 7.5 ms (17.7) | 5.4 ms (24.8) | 12.9 | 10.3 (10.2) |
| GL map/unmap PBO | down | 28.7 ms (4.6) | 5.1 ms (25.9) | 33.8 | 3.9 (3.9) |
| GL direct client ptr | up | - | - | 12.6 | 10.5 (10.2) |
| GL direct client ptr | down | - | - | 24.8 | 5.4 (5.3) |
| Vulkan staging+image | up | 10.3 ms (12.9) | 5.3 ms (24.8) | 15.7 | 8.5 (8.1) |
| Vulkan staging+image | down | 24.0 ms (5.5) | 5.1 ms (26.2) | 29.1 | 4.6 (4.5) |
| Vulkan staging<->device buffer | up / down | - | 5.6 ms (23.5) / 4.9 ms (26.9) | - | p90 23.1 / 26.5 |
| Vulkan ReBAR direct write | up | 7.1 ms (18.7) | none (data already in VRAM) | 7.1 | 18.7 (17.4) |
| Vulkan ReBAR direct read | down | ~200 ms per 8 MB slice | - | impractical | ~0.04 |

Note 1: with a write-only persistent mapping radeonsi places the PBO in VRAM behind the BAR, so the "memcpy" is already the PCIe write and the following `pbo->tex` is a VRAM-local copy. Treat the 7.5 ms total as the true upload cost; the 0.52 ms DMA figure is not a PCIe number.

## Results: all formats, median GB/s (p90 GB/s), by stage

| Path / stage | float32 | half | 8-bit |
|---|---|---|---|
| Frame size | 132.7 MB | 66.4 MB | 33.2 MB |
| GL persistent up: memcpy | 19.0 (18.3) | 20.2 (15.6) | 19.2 (17.2) |
| GL persistent up: total | 17.7 (17.0) | 18.1 (14.2) | 16.5 (14.9) |
| GL persistent down: DMA | 26.0 (25.8) | 25.6 (24.9) | 24.9 (23.8) |
| GL persistent down: memcpy | 6.8 (6.7) | 13.8 (8.8) | 22.6 (14.2) |
| GL persistent down: total | 5.4 (5.4) | 9.0 (6.5) | 11.7 (9.0) |
| GL map/unmap up: DMA | 24.8 (24.4) | 23.0 (22.3) | 20.0 (19.8) |
| GL map/unmap up: total | 10.3 (10.2) | 10.1 (9.8) | 11.7 (10.9) |
| GL map/unmap down: total | 3.9 (3.9) | 8.6 (8.4) | 10.4 (9.9) |
| GL direct up | 10.5 (10.2) | 12.5 (12.3) | 11.7 (11.3) |
| GL direct down | 5.4 (5.3) | 10.3 (8.4) | 13.1 (11.5) |
| VK staging up: memcpy | 12.9 (11.9) | 20.4 (19.4) | 25.1 (23.1) |
| VK staging up: DMA | 24.8 (24.4) | 23.1 (22.4) | 19.7 (19.6) |
| VK staging up: total | 8.5 (8.1) | 10.8 (10.5) | 11.1 (10.8) |
| VK staging down: DMA | 26.2 (25.8) | 25.9 (25.2) | 25.4 (25.1) |
| VK staging down: memcpy | 5.5 (5.4) | 12.8 (12.0) | 17.6 (14.1) |
| VK staging down: total | 4.6 (4.5) | 8.6 (8.2) | 10.4 (9.0) |
| VK buf->devbuf up (DMA) | 23.5 (23.1) | 23.4 (22.7) | 22.0 (20.8) |
| VK devbuf->buf down (DMA) | 26.9 (26.5) | 26.6 (25.9) | 25.9 (24.7) |
| VK ReBAR direct write | 18.7 (17.4) | 22.4 (22.1) | 19.1 (18.6) |

## Multithreaded host copy (8 threads, float32 unless noted)

Same buffers as above, `memcpy` split across 8 `std::thread`s (spawned per call, ~0.1 ms overhead included).

| Stage | x1 ms (GB/s) | x8 ms (GB/s, p90) | Notes |
|---|---|---|---|
| VK upload memcpy->staging | 9.5 (14.0) | 5.5 (24.3, 22.8) | half 20.4 -> 26.2, 8-bit 25.1 -> 26.7 |
| VK download staging->memcpy | 24.9 (5.3) | 8.8 (15.0, 14.6) | half 12.8 -> 19.0, 8-bit 17.6 -> 34.2 |
| GL persistent upload memcpy->pbo | 8.6 (15.4) | 6.3 (21.2, 20.8) | half 19.9 -> 20.2, 8-bit 18.7 -> 16.3 (spawn overhead) |
| GL persistent download pbo->memcpy | 23.4 (5.7) | 8.7 (15.2, 14.0) | half 13.0 -> 19.0, 8-bit 20.7 -> 34.5 |
| GL client-storage readback PBO x1 / x8 | 23.6 (5.6) | 9.1 (14.6, 13.3) | no gain from `GL_CLIENT_STORAGE_BIT`; same as plain PBO |
| host->host memcpy (baseline) | 9.6 (13.9) | 8.2 (16.2) | reference for what RAM itself allows |

Float32 UHD per-frame total (ms) with 8-thread host copy: Vulkan up 5.5 + 5.3 DMA = 10.8, down 5.1 + 8.8 = 13.9; GL persistent up 6.3 + 0.6 = 6.9, down 5.1 + 8.7 = 13.8. For GL up the PCIe write happens inside the memcpy (see note 1).
Small frames (8-bit) gain little or lose because thread spawn costs are comparable to the copy; a persistent thread pool would be needed there. A GL map/unmap row was not re-measured with threads.

## Per-frame cost (ms, total incl. CPU memcpy, no overlap)

| Format | GL persistent up / down | GL map/unmap up / down | VK staging up / down | VK ReBAR write up | Pure DMA each way (VK) |
|---|---|---|---|---|---|
| float32 (132.7 MB) | 7.5 / 24.6 | 12.9 / 33.8 | 15.7 / 29.1 | 7.1 | 5.3 / 5.1 |
| half (66.4 MB) | 3.7 / 7.4 | 6.6 / 7.7 | 6.1 / 7.8 | 3.0 | 2.9 / 2.6 |
| 8-bit (33.2 MB) | 2.0 / 2.8 | 2.8 / 3.2 | 3.0 / 3.2 | 1.7 | 1.7 / 1.3 |

## Findings

- The link itself is fast and symmetric: ~25-27 GB/s per direction of real DMA (~80% of PCIe 4.0 x16), identical for GL and Vulkan; both stacks hit the hardware ceiling, so the API choice does not change raw transfer cost.
- The host side dominates, not the DMA. Single-threaded `memcpy` into the write-combined upload staging runs ~13-20 GB/s; single-threaded `memcpy` out of the download staging runs only 5-6 GB/s for float32 (13-21 GB/s for smaller frames), so a single-threaded download costs ~4-5x its DMA time.
- Cause of the slow download copy (checked): it is not a missing HOST_CACHED type. The Vulkan download staging was already HOST_VISIBLE|HOST_COHERENT|HOST_CACHED (RADV type 5, flags 0xe; coherent, so no `vkInvalidateMappedMemoryRanges` needed), and the GL readback PBO behaves identically with `GL_CLIENT_STORAGE_BIT` (5.6 GB/s x1). A plain host->host `memcpy` of the same size does 13.9 GB/s single-threaded, so reading GPU-written (DMA'd, snooped) pages with one thread is ~2.5x slower than ordinary RAM, probably per-core read-latency/prefetch limits on that mapping. It is a single-core limit, not an uncached-memory one: 8 threads lift it to ~15 GB/s, equal to the host->host x8 figure (16 GB/s). Not isolated further.
- Run-to-run noise on the CPU memcpy stage is large (Vulkan float32 upload memcpy was 7.5 ms in one run and 10.3 ms in another, from different page placement); DMA stages repeat within ~1%.
- Half halves and 8-bit quarters every cost; half is the natural wire format between GPU nodes if precision allows.
- ReBAR direct write removes the staging copy and DMA step entirely but is CPU-write bound (~19-22 GB/s); CPU reads from it are unusable (~40 MB/s), so it helps upload only.
- `vkCmdCopyBufferToImage` on an optimal-tiling image is not slower than a raw buffer copy (24.8 vs 23.5 GB/s), so tiling/detiling is not a bottleneck.

## What this means

A UHD float32 RGBA frame costs roughly 5 ms of pure PCIe DMA each way, 7-16 ms to upload and 25-34 ms to download once a single-threaded host copy is included, falling to ~7-11 ms up and ~14 ms down with an 8-thread copy (half precision: 3-8 ms each way single-threaded). Per node, a round trip of ~30-40 ms at float32 would swamp any node cheaper than that, so the backend must keep frames resident on the GPU across consecutive GPU nodes and transfer only at graph boundaries (source read, viewer/write, CPU-only nodes); the DMA alone (~5 ms) is small enough that boundary transfers are acceptable, and pipelining upload/download behind compute on a transfer queue can hide most of it. Whether per-node transfer pays off versus CPU execution is a comparison against heavy-node CPU cost per UHD frame, which is filled in later by comp profiling: compare against the "~5 ms DMA" and "7-34 ms with host copy" figures above (heavy nodes at hundreds of ms per frame would amortise even a full round trip; nodes at <30 ms would not). Vulkan gives no raw-transfer advantage over GL here, so the choice should rest on compute/control, not transfer speed.

## Not measured

- Pipelined/overlapped transfers, a dedicated async transfer (SDMA) queue, and pageable-pointer upload through Vulkan (`VK_EXT_external_memory_host`).
- `lspci -vv` LnkSta (needs root); PCIe link taken from sysfs `current_link_speed/width` (read while GPU may be at idle link state, but it reports Gen4 x16).
- GL path used a compatibility context via EGL; Natron's actual PBO code was not run.
