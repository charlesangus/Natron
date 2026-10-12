# Clean-sheet Merge

Merge is rewritten as native `fr.natron.Merge` (M101 - Clean-Sheet Merge Node), with Nuke's formulas made HDR-safe (no legacy toggle), layer modes (Single / All matching / List) and per-side alpha sources (Universal / Per-layer / Per-channel alpha layer). Dropped: alpha masking, mask fringe and inject, video colorspace. The old Merge IDs (`net.sf.openfx.MergePlugin` and presets) are deleted with no compatibility; old projects get the missing-plugin node. M85 - Native GPU Kernels no longer ports Merge. User decisions 2026-10-12.
