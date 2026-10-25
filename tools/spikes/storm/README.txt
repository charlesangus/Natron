Storm context spike (throwaway, not shipped). Build in natron-dev:
  cmake -S tools/spikes/storm -B build/spikes/storm -G Ninja -DCMAKE_PREFIX_PATH=/usr/local && ninja -C build/spikes/storm
Run (llvmpipe, build/deeprepro Xvfb recipe env; USD plug-ins load from /usr/local/lib/usd):
  build/spikes/storm/m19spike Tests/fixtures/scene-reference.usda build/spikes/storm-out/llvmpipe
Run on AMD (radeonsi) and NVIDIA hosts the same way with a real DISPLAY, out dirs build/spikes/storm-out/{radeonsi,nvidia}.
Each run prints a REPORT block (context version/profile received, GL_VERSION/RENDERER, whether
Storm and the immediate-mode widget both drew) and writes storm.png, compat.png, window.png.
--force-version 2.1 checks what a below-4.5 request yields.

llvmpipe results 2026-10-25 (Qt 6.8.3, Mesa 25.2.7, run-llvmpipe.sh):
  default request 4.5 compat -> Qt gets a 4.5 CORE context (GL_VERSION "4.5 (Core Profile)"); no 4.5 compat
    functions, immediate mode draws nothing, Storm floods HgiGL "invalid enum/operation" errors and draws nothing.
  --force-version 3.3 -> same as above (core).
  --force-version 2.1 -> 4.5 COMPATIBILITY context; Storm draws (cube visible, unlit/black: no lights set),
    immediate-mode widget draws, contexts share, zero GL errors. Output in llvmpipe-2.1/.
