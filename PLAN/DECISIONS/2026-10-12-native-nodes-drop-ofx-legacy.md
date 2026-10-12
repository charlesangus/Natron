# Native nodes drop OFX legacy

User decisions, 2026-10-12, after an audit of the native nodes ported from OFX in M67 - Native Core Nodes:

- **The parity rule is retired.** Native nodes no longer copy their OFX plug-in's knob names, order, defaults or maths (`Engine/Nodes/README.md`'s "Host knobs and script names", "IDs and versions" and "Siblings" sections). They are designed on their own merits. Parity fixtures become regression fixtures, re-recorded when behaviour changes on purpose. This follows from `2026-09-18-clean-break-no-project-compat`: with no old projects to load, parity protected nothing.
- **Plain IDs.** Native nodes take plain IDs (`Grade`, `Merge`, `Shuffle`, `DeepMerge`, `Read`) at major 1. Reverse-domain IDs (`net.sf.openfx.*`, `net.sf.cimg.*`, `eu.cimg.*`, `fr.natron.*`, `fr.inria.built-in.*`) are for OFX plug-ins running in an arbitrary host. They are all renamed at once (M107 - Native Node Identity Cleanup), with no compatibility.
- **One node per feature, no duplicate menu entries.** Transform and TransformMasked become `Transform`, Erode and Dilate become `ErodeDilate`, and Constant and Solid become `Constant`. None keep a menu entry for the old variant. **Exceptions:** Add, Multiply and Gamma stay separate nodes ("useful to see in the node graph that this is an add node"), and Merge keeps its operation presets (Plus, Screen…) as menu entries.
- **Clamps:** `clampBlack`/`clampWhite` on Grade, ColorCorrect and Saturation stay, but default off (M108 - Native Node Behaviour Fixes).
- **ChromaKeyer** joins M95 - Knob Cleanup And Keyer Redesign as a scene-linear fix: no Rec.709 encoding, no clamps, host mask/mix.
- **Blur** is trimmed to a Nuke-like knob set: size, filter, crop to format. orderX/Y, alphaThreshold, boundary, expandRoD and uniform are removed (M108).
