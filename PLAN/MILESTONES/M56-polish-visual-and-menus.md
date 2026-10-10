# M56 - Visual And Menu Polish

Full title: Polish — visual and menus

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

A consolidated milestone for independent visual-polish work:
- Node text layout: grow horizontally to a max width, then wrap vertically
  instead of overflowing.
- Icon replacement pass: many icons are hard to read; replace/redesign for
  legibility.
- Stylesheet/look-and-feel overhaul: revisit spacing, density and visual
  hierarchy — panels feel cramped and cluttered.
- Premult/Unpremult's unpremult-by channel picker has no label (the plugin
  never labelled it either); give it one. (User, 2026-09-30, from M65 T5f.)
- On IDistort and STMap the "V Channel" picker is indented relative to
  "U Channel"; align it. (User, 2026-09-30, from M65 T5f.)

Blocked on: needs an icon audit and a design direction/spec before
elaboration (user request 2026-09-18, consolidated 2026-09-18 — see the
reorg decision).

Acceptance sketch:
- Long node names wrap instead of overflowing the node silhouette.
- A defined set of hard-to-read icons is replaced with clearer versions.
- Key panels (node graph, properties, viewer) read as less cramped, judged
  against before/after screenshots.
- The unpremult-by picker is labelled, and IDistort/STMap's U/V/A channel
  pickers line up.

Scoping draft (2026-10-10): `PLAN/DESIGN/2026-10-10-m44-m55-m56-scoping.md` holds a draft phase/task breakdown and the user questions to settle before elaborating. Deps: M24 (confirmed by the user).
