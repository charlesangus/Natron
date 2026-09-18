# Backlog reorg and prioritization (2026-09-18)

After a `/cat-discuss` review of the full inbox backlog (13 new stub
milestones plus M51–M54) and the existing `todo` board rows, the user set
this priority order, ahead of resuming the 3D roadmap:

1. **Channel/layer rework** (sequential): M39 (rename "planes"→"layers")
   → M34 (new native Shuffle node) → M35 (remove implicit shuffling
   elsewhere, blocked on M34) → M36 (new-channel/layer affordance) → M37
   (channel/layer add/remove nodes) → M38 (layer UI reorg, blocked on M39)
   → M50 (OCIO as a first-class project property).
2. **Compositing semantics**: M43 (drop the premultiplied/unpremultiplied
   concept).
3. **Infra/housekeeping**: M25 (debug FP trap guard), M27 (fix debug build
   defining NDEBUG), M28 (memfree fix), M22 (lossless round-trip with
   missing plugins — placed here as general project-file robustness, not
   3D/deep-specific), M29 (independent repository), M30 (automated beta
   releases).
4. **Polish**: M24 (category colour, gains a white-on-dark label task) and
   M44 (trackball colour editing) stay separate from two new *consolidated*
   stub milestones — M55 (node-graph interaction: input-pipe visibility,
   splice-on-drop, roto feather multi-select, render dialog rework) and M56
   (visual/menus: node text wrap, icon pass, stylesheet overhaul) — rather
   than one milestone file per backlog item, to ship related low-risk fixes
   together.
5. **3D roadmap** (unchanged internal sequencing, now resumes after the
   above): M19 → M20 → M52 → M53 → M54.

**Deferred, no active order** — explicitly parked by the user: outstanding
deep-compositing work (M21, M51, and the M32 deep-sample-inspector stub),
porting the user's Nuke tools (M45 tabtabtab-nuke, M46 Labelmaker), the
`.ntp` format redesign (M16), and the architectural-cleanup parking lot
(M31).
