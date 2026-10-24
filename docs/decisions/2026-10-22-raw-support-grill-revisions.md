# RAW support: grill revisions

Decided 2026-10-22 with the user, grilling M80 - Native RAW Support (`/cat-plan --grill`). This revises `2026-10-09-raw-support-via-rawspeed-and-darktable-ports.md`.

- **GPU split.** The core chain's GPU kernels now ship in M80 - Native RAW Support: the mosaic in GPU regions, Prepare, Temperature, Highlights, Demosaic PPG and RCD, and InputColor. M81 - Raw GPU Kernels keeps AMaZE, X-Trans, hot pixels, CA and denoise. This follows the 2026-10-13 round-two rule that ports ship their own kernels, now that the backend is settled.
- **Lens correction** becomes its own milestone, M160 - Lensfun Lens Correction: lensfun, any image, metadata-aware, on M113 - Native Warp Nodes' coordinate sampler. It is separate from M132 - Native Lens Distortion.
- **Raw kind** covers linear and no-CFA files as well (3 or 1 channels). A RawToImage node is the plain explicit converter, and the Viewer accepts raw directly as it does deep.
- **Develop RAW** is loose nodes in a backdrop, auto-inserted only on interactive creation, permanently. M159 - Kind-Agnostic Groups removes the groups-are-an-image-barrier limitation in general, for every kind.
