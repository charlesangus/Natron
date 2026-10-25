# darktable AI tools and image-tool ports

Decided 2026-10-25 with the user in a /cat-plan session ("what else is worth taking from darktable", display-referred modules excluded).

- **AI tools get their own epic, E16 - AI Tools.** It is built on an ONNX Runtime backend modelled on darktable 5.6's `src/ai/` (PR #20322): ONNX Runtime is loaded with `dlopen`, and the execution provider is found at runtime. Auto mode tries CUDA, then MIGraphX, then ROCm, then CPU. Inference sits beside the Vulkan backend, not inside it. Tensors go through host memory, and AI nodes end GPU regions. The CPU build of ONNX Runtime ships in the AppImage. GPU builds are installed afterwards into the user data dir, the way darktable's `tools/ai/install-ort-gpu.sh` does it.
- **Models are downloaded on demand, never bundled.** They are fetched from darktable-ai's `.dtmodel` release assets through a pinned manifest, checked against a sha256 and stored in the user data dir. darktable-ai's rule applies: weights must carry a GPL-3-compatible licence, and every model's weight licence and training-data licence are shown before download.
- **AI consumers.**
  - The object mask traces into editable Roto beziers. It runs one shot on the current frame, with no per-frame segmentation and no SAM2 video propagation. SegNext (MIT) is the default model and SAM 2.1 is optional, with SAM 2.1's research-only SA-1B training data flagged.
  - RawNIND raw denoise.
  - Neural denoise and upscale. The sRGB-trained models run through an invertible range wrapper so values above 1 survive, and the RawNIND linear model is the scene-linear default.
- **Classical ports get their own epic, E17 - darktable Image Tools:**
  - a shared edge-aware filter library
  - profiled denoise with darktable's noise-profile database
  - wavelet retouch on Roto shapes
  - tone equalizer
  - contrast & texture
  - color balance rgb
  - color equalizer
  - ashift auto-straighten

  Each port carries every user-facing control and darktable's defaults, plus a params-to-knob audit (`port-means-every-control`). Each is checked against darktable-cli renders from the M80 - Native RAW Support reference harness at `DARKTABLE_REF`.
- **Not taken:**
  - Diffuse or sharpen, haze removal, RGB CA correction, liquify, bilateral (M130 owns one) and the blurs module.
  - darktable's display-referred modules.
  - The MCP server, the Lua AI API, and auto-tagging, which is unreleased.
