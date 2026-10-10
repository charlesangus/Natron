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

## Decisions

- 2026-10-10 — **Icon direction (user):** a monochrome SVG line-icon set under a GPL-compatible licence, recoloured by the theme, with the three GroupingIcons sets reduced to one. The user wants pitched options before any before/after work starts.
- 2026-10-10 — **Icon set picks (user, from the icon-options page):** Lucide is the set, drawn at a 1.5 px stroke (native 2 px is too heavy). Overrides for the toolbox grouping icons: Filter should look like Tabler's or Phosphor's filter glyph, drawn in-house in Lucide's style if Lucide has nothing suitable; Channel is a "hamburger" (three stacked horizontal lines); Merge uses the layers icon the page proposed for Deep; Deep gets a custom "D" glyph in Lucide's style. The rest of the page's questions (toolbox tint, B&W setting, NativeNodes/merge-operator scope, licence route) are still open; Lucide's ISC licence keeps GPL-2.0-or-later.
- 2026-10-10 — **Toolbox glyph selects (user):** Filter is the custom droplet-with-fins glyph ("Filter v2", after Phosphor's `drop-half`); Deep is the custom D inside a rounded square ("Deep v3", Lucide's `square-*` convention); Keyer is Lucide `pipette` rather than `key-round`, since a key reads as keyframes while a keyer picks a colour. Channel is Lucide `menu` and Merge is Lucide `layers-2`. All custom glyphs use Lucide's 24 px grid, round caps and joins, at the 1.5 px stroke. The drafts' SVG markup is in the icon-options page (claude.ai artifact `9B52JMKTyWw5Ufep99afxr`, "Chosen" section).
