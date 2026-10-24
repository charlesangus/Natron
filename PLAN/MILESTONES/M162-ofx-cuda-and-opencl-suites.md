# M162 - OFX CUDA And OpenCL Suites

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Host the OpenFX 1.5 CUDA and OpenCL render suites, so third-party compute plugins join a resident region with no host round trip. Port OFX 1.5's `ofxGPURender.h` properties into the `libs/OpenFX` fork: CUDA stream support, OpenCL buffers and images, the optional `OfxOpenCLProgramSuiteV1`, and `kOfxStatGPUOutOfMemory`. Extend HostSupport's render in-args with the CUDA stream. Advertise each suite only when its backend is live. Import exported Vulkan memory and semaphores, through a `dlopen`ed `libcuda.so.1` (driver API: `cuImportExternalMemory`/`cuImportExternalSemaphore`/`cuWait|SignalExternalSemaphoresAsync`) or through OpenCL `cl_khr_external_memory_opaque_fd` + `cl_khr_external_semaphore_opaque_fd`, with the device matched by `cl_khr_device_uuid`. Hand the plugin `kOfxImagePropData` as a device pointer or `cl_mem`, pass the host stream or queue, and never wait for the plugin's work. Reuse M86 - OFX 1.5 GPU Suites' region boundary. Carries M84 - GPU Placement And Residency's hand-offs: an exportable `GpuBufferPool` variant, imported once per buffer and recycled; out-of-pool plugin memory, checked against the 1 GiB `kGpuStripOutsidePoolHeadroom` (moved here from M86 - OFX 1.5 GPU Suites); `kOfxStatGPUOutOfMemory` mapped onto the evict → serial strips → halve → CPU ladder.

Filed 2026-10-22 at the user's request, while M86 - OFX 1.5 GPU Suites was scoped to the GL suite only.

Blocked on: M86 - OFX 1.5 GPU Suites (the region boundary) and its P1.T1 spike (which compute runtimes and extensions exist on the dev host and CI); user calls on CUDA-without-NVIDIA (consultant recommends: build it, test without a driver, validate on M91 - NVIDIA GPU Validation's host) and on a staged OpenCL fallback (consultant recommends: zero-copy where available plus a staged RAM path outside regions, tested on CI with EPEL 9's pocl 3.1); end-to-end CUDA validation needs M91 - NVIDIA GPU Validation's host.

Acceptance sketch:
- An OFX 1.5 OpenCL sample plugin (`libs/OpenFX/Examples/OpenCL`) renders inside a resident region with no host round trip wherever a zero-copy OpenCL runtime exists, and elsewhere follows the agreed fallback.
- The CUDA path builds in CI, is unit-tested without a driver (clean unavailability, suite not advertised), and is validated end to end on M91 - NVIDIA GPU Validation's NVIDIA host.
- A plugin returning `kOfxStatGPUOutOfMemory` takes the out-of-memory ladder, and out-of-pool memory with real plugins is measured against the headroom.
