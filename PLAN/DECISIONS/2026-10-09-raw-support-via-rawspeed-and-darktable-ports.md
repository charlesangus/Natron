# RAW support returns via rawspeed and darktable ports

Decided 2026-10-09 with the user in a /cat-plan session.

- RAW camera files come back natively. The decoder is **rawspeed**, vendored as a pinned dependency; LibRaw is not used.
- The native Read emits the **undemosaiced sensor mosaic** on a new raw data kind. It is not a separate RawRead node, and not a pre-demosaiced image. If a source node cannot resolve its output kind from the file (M80.P2.T2), the fallback is a separate RawRead node and the user must be asked.
- The darktable modules ported are the core chain (rawprepare, temperature, highlights, demosaic, input colour) plus extras: hot pixels, chromatic aberrations, raw denoise and lens correction.
- GPU is in scope but split into M81 - Raw GPU Kernels, so the CPU work in M80 - Native RAW Support can ship first. M80 depends on M75 - Native Read shipping.

This supersedes the "RAW camera formats are dropped" clause of `2026-10-09-native-read-format-scope.md`; the "different technique" it anticipated is rawspeed plus these ported nodes.
