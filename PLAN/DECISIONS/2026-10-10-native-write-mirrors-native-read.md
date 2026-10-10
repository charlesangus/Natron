# Write mirrors the Read: all OIIO formats, container removed

Decided 2026-10-10 with the user, at the promotion of M76 - Native Write.

The native Write writes every format OpenImageIO can write, taken from OIIO's runtime format list with the same exclusions as the Read (RAW, null, term, ffmpeg, psd). `WriteNode` (the OFX Write container) and the OFX writers are removed, with no extension-based fallback. Video stays unsupported until a native video writer exists. This confirms the expectation recorded in `2026-10-09-native-read-format-scope.md` and supersedes the "video on the OFX writer through a fallback" clause of `2026-10-08-native-io-and-metadata-design.md` for the Write side.
