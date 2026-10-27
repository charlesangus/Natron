# M246 - Vulkan Inference Provider

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Adds GPU inference on any Vulkan device by loading [onnx-vulkan-rs](https://github.com/automataIA/onnx-vulkan-rs) (Apache-2.0/MIT) as an ONNX Runtime plugin execution provider. The project is a Rust shared library loaded through ORT's Plugin EP API (ORT 1.23 and later), so M203 - ONNX Inference Backend's `dlopen`ed ORT registers it with `RegisterExecutionProviderLibrary` and nothing else changes. It gives AMD users without ROCm, and NVIDIA users without CUDA/cuDNN, GPU inference on a device Natron already requires. The plugin is user-installed, like M203's GPU ORT builds, so Rust never enters our build.

Blocked on: upstream maturity. At 2026-10-27 the project has 45 commits, no releases, fp16 parked and AMD unvalidated. Elaborate once it publishes versioned releases and fp16 lands, and after M204 - AI Model Manager, so the E16 - AI Tools models it must run are known.

Acceptance sketch:
- A `tools/ai/` installer, modelled on M203's `install-ort-gpu.sh`, fetches a pinned plugin release, checks its sha256 and unpacks it into the user data dir.
- The loader registers the plugin when present; Vulkan appears in Preferences' Device list and detected providers, and has a slot in the Auto order.
- Every E16 - AI Tools model runs on the Vulkan EP within a recorded tolerance of the CPU EP, or is listed as refused with the offending nodes named; refused models fall back to CPU.

## Scope

**In:**
- The installer, plugin registration, Device and Auto-order entries, and a per-model validation against the CPU EP.

**Out:**
- Building or vendoring onnx-vulkan-rs, or any Rust toolchain in our build.
- Zero-copy sharing between Natron's Vulkan device and the plugin's: no milestone owns it. Tensors go through host memory, as with CUDA in M203 - ONNX Inference Backend.
- Bundling the plugin in the AppImage.

**Edge cases:**
- **Symbolic input dimensions:** the plugin needs concrete shapes, so each tile size needs its own session. M203's tiler halves the tile on out-of-memory, and the session cache key must carry the input shape for this EP.
- **Whole-graph refusal:** the plugin refuses a graph with any unsupported op at load time, with no per-node fallback. Session creation then falls back to CPU through M203's existing EP-fallback path, with the refusal logged.
- **Ops our models use** (ConvTranspose, Resize, DepthToSpace among them) are unverified upstream; the validation decides per model.
- **A second Vulkan device in the process:** the plugin creates its own device beside Natron's. Upstream's NVIDIA tests must run serially because parallel device creation hung the driver, so creation is serialised and tested under concurrent renders.

**Assumptions:**
- ORT's Plugin EP API stays compatible across the ORT version M203 pins and the one the plugin is built against (upstream tests 1.29.0; M203 pins 1.31.1).
