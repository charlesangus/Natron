# M382 - Nucleon Logo And Artwork

Design the Nucleon logo and produce every piece of art the app ships with: the app icon set, the project-file icon, the startup and project-loading splashes, and the About-dialog logo. Start with four agent-drafted SVG concepts. The user picks one and refines it over a couple of rounds, and the chosen mark is then rendered into every size the app needs from one script. This is the one F17 - OpenNucleon Flag Day member that may start before the switchover. It merges into the Feature branch only, as files nothing references yet. M386 - Nucleon Branding And Packaging wires it in.

Context:
- **Art being replaced** (all in `Gui/Resources/Images/`, registered in `Gui/GuiResources.qrc`):
  - `splashscreen.png`, 767x465 grayscale, with its source `splashscreen.svg`
  - `loadProjectSplashscreen.png`, 768x301
  - `natronIcon256_linux.png`, the app and window icon, which `Gui/AboutWindow.cpp` also shows at 128 px
  - `natronProjectIcon_linux.png`, the MIME icon
  - the `natronIcon.svg` master, plus `natronIcon.pdf`, `natronIcon.icns` (dead) and `natronIcon-README.txt`
  - `Images/Other/natron_picto_{tools,viewer}.svg`, which nothing references
- **Text overlays are drawn by code.** `Gui/SplashScreen.cpp` scales the pixmap and draws the status text at (120, 100) and the version at (20, height − 15). The project splash draws at (250, height − 51) and (300, height/2). Each new splash must keep a clear, dark band where M386 - Nucleon Branding And Packaging will place that text. Record the band rectangles in `Gui/Resources/Branding/README.md`.
- **Renderer:** no `rsvg-convert` or Inkscape on the host. Use CairoSVG (`pip install --user --break-system-packages cairosvg`), the same inside or outside natron-dev, so renders are reproducible.
- **Screenshots go to the user one file each** via SendUserFile, never as a contact sheet.

## Scope

**In:**
- Four logo concepts, each a distinct direction:
  1. an abstract nucleus of clustered spheres
  2. an orbital ring
  3. a geometric N monogram
  4. a node-graph motif
- Each concept is a flat mark plus a "Nucleon" wordmark.
- Up to two refinement rounds on the chosen concept.
- The final set:
  - mark and wordmark SVG masters
  - app icon PNGs at 16, 24, 32, 48, 64, 128, 256 and 512 px
  - a project-file icon at the same sizes
  - startup splash at 1x (768x464) and 2x
  - loading-project splash at 1x (768x300) and 2x
  - About logo at 128 and 256 px
  - a render script and a README
- Splash direction (user): the mark plus the wordmark on a dark background with a subtle accent gradient, and a reserved text band.

**Out:**
- Wiring the art into qrc, code, the desktop file or the AppImage: M386 - Nucleon Branding And Packaging.
- Deleting the old Natron images: M386 - Nucleon Branding And Packaging.
- Replacing the UI icon set in `Gui/Resources/Icons/natron/`, which is product-neutral. The directory rename belongs to M385 - OpenNucleon Code Rename.
- A website or social-media art.

**Edge cases:**
- **Small sizes.** The mark must stay legible at 16 px. Concepts that turn to mush at 16 px are flagged in the concept round rather than dropped silently. A simplified small-size variant SVG is allowed.
- **Fonts.** Convert the wordmark to outlined paths so no font is needed at runtime. If it starts from a font, that font must be OFL or similarly GPLv3-compatible; record it in the README and in `tools/license/components/`.
- **Light and dark.** Every concept render shows the mark on both a dark (#1e1e1e) and a light (#f0f0f0) background, because desktop icon themes vary.

**Assumptions:**
- Unconfirmed: two refinement rounds are enough; a third goes back to the user as a `response` question rather than continuing.
- CairoSVG renders the SVG features the concepts use (gradients, clip paths). Concepts avoid filters such as blur, which CairoSVG handles poorly.

## Phase 382.1: Concepts

- [x] M382.P1.T1 — Draft four logo concepts as SVG
  - files: `Gui/Resources/Branding/concepts/{nucleus,orbital,monogram,graph}.svg` (new)
  - approach: Hand-write four SVGs, one per direction in `## Scope`. Each file holds the mark alone (square viewBox 0 0 512 512) and a horizontal lockup with the "Nucleon" wordmark as outlined paths. Use flat shapes and at most three colours, with a palette that works on both dark and light backgrounds. Keep geometry simple enough to read at 16 px. No filters or embedded bitmaps.
  - verify: `python3 -c "import cairosvg"` renders each file without error, and `xmllint --noout` passes on all four.
  - size: L

- [x] M382.P1.T2 — Render the concept sheets and send them for a pick
  - files: `tools/branding/render.py` (new), output under `build/branding/concepts/` (untracked)
  - approach: `render.py` uses CairoSVG to render any SVG to PNG at a list of sizes, over a given background colour. For each concept, render the mark at 16, 32 and 256 px on dark and on light, plus the lockup at 1024 px wide on dark. Send one PNG per concept-and-background via SendUserFile, then put the pick to the user as a `response` question under `# Open questions`.
  - verify: `render.py` exits 0 and produces the expected files. The pick is recorded in this milestone's `## Decisions`.
  - size: M

## Phase 382.2: Refinement

- [ ] M382.P2.T1 — Refine the chosen concept per the user's feedback
  - files: `Gui/Resources/Branding/nucleon-mark.svg`, `Gui/Resources/Branding/nucleon-lockup.svg`, `Gui/Resources/Branding/nucleon-mark-small.svg` (optional), all new
  - approach: Promote the picked concept to the masters, applying the user's notes. Add a small-size variant only if the 16 px render loses detail. Re-render with `render.py`, send the renders, and ask again. Repeat for at most two rounds. Record each round's notes in `## Decisions`.
  - verify: The user approves the mark and the lockup; the approval is recorded in `## Decisions`.
  - size: M

## Phase 382.3: Final Art Set

- [ ] M382.P3.T1 — Build the icon set and the project-file icon
  - files: `Gui/Resources/Branding/nucleon-project.svg` (new), `Gui/Resources/Branding/icons/` (new PNGs), `tools/branding/render.py`
  - approach: Draw the project-file icon as a document glyph carrying the mark. Extend `render.py` with a `--set` mode that renders the app icon and the project icon at 16, 24, 32, 48, 64, 128, 256 and 512 px, using the small-size variant for 16 to 32 px when it exists. Name the files `nucleon-<size>.png` and `nucleon-project-<size>.png`. Commit the PNGs, which are small, so the build doesn't need CairoSVG.
  - verify: `render.py --set` regenerates byte-identical PNGs from the committed SVGs (run it twice and compare the `sha256sum`s). `file` reports the expected dimensions for every PNG.
  - size: M

- [ ] M382.P3.T2 — Build the two splashes and the About logo
  - files: `Gui/Resources/Branding/splash.svg`, `Gui/Resources/Branding/splash-loading.svg`, `Gui/Resources/Branding/splash{,@2x}.png`, `Gui/Resources/Branding/splash-loading{,@2x}.png`, `Gui/Resources/Branding/about-{128,256}.png` (all new), `tools/branding/render.py`
  - approach:
    - Startup splash: 768x464 viewBox, with the lockup on a dark background with a subtle accent gradient.
    - Loading splash: the same design at 768x300.
    - Keep a clear band for the code-drawn text in each, sized to hold the existing strings at the existing font size. Record each band's rectangle in px.
    - Render both at 1x and 2x, and the About logo from the mark at 128 and 256 px.
    - Send the two 1x splashes to the user for approval.
  - verify: `render.py` reproduces the PNGs byte for byte. The user approves both splashes (recorded in `## Decisions`).
  - size: M

- [ ] M382.P3.T3 — Document the art set and record the licences
  - files: `Gui/Resources/Branding/README.md` (new), `tools/license/components/README.md`
  - approach: The README lists every file, what it is for and which current Natron asset it will replace. It gives each splash's text-band rectangles, the palette hex values, the render command (`python3 tools/branding/render.py --set`) and the font provenance. Add the wordmark font to the component list if one was used. The art itself is GPL-3.0-or-later with the project.
  - verify: Every file under `Gui/Resources/Branding/` is named in the README, checked with a short `ls` and grep loop in the PR description.
  - size: M

**Verification gate:**
- `python3 tools/branding/render.py --set` run twice yields identical `sha256sum`s for every committed PNG.
- `xmllint --noout Gui/Resources/Branding/*.svg` passes.
- No file outside `Gui/Resources/Branding/`, `tools/branding/` and `tools/license/components/` changed.
- The user has approved the mark, the lockup and both splashes (recorded in `## Decisions`).
- The PR targets `feature/f17-opennucleon-flag-day`, never `main`.

## Decisions

- 2026-11-10 — First concept round rejected (user): none of nucleus, orbital, monogram or node graph worked. Second round: four new clean-sheet designs drawn from socialist-realist art, retro-futurist/atompunk, existing and past DCC logos, eighties aesthetics and Bauhaus; interesting and unique but obeying design principles; a greyscale palette, optionally with one or two colours from the default theme (highlight orange, keyframe blue). Drafted by a Fable agent (user). The first concepts are removed from the branch and are not shown to the new drafter.
- 2026-11-10 — Round 2 feedback (user):
  - Favourites are quarks and wedge.
  - Wordmarks: likes the strata and quarks lettering, not wedge's.
  - Quarks: reads poorly on dark. Explore other arrangements of the three dots and other enclosing shapes instead of the right-facing triangle, e.g. the orange dot at the bottom with the two above as if merging into it, or a round enclosure.
  - Wedge: too busy, drop the square tile, poor at small sizes. Keep the Bauhaus/Kandinsky feel with simpler variations.
  - Also try Bauhaus/Kandinsky meets Feynman diagram.
  - Round 3 is variations on these directions, drafted by a Fable agent.
- 2026-11-10 — Round 3 feedback (user):
  - Quarks: keep all three dots the same size. Try orange at bottom-left with one dot directly above and one to its right, plus the mirror image. The user likes the triangle, so try these inside a triangle.
  - Wedge-split variations: the wedge going right through, a more extreme split, the wedge passing through the split.
  - Feynman: liked but too simplistic and unbalanced. Revisit as more Bauhaus, more exaggerated and stylised, less literally a Feynman diagram.
  - Round 4 goes to the Fable agent.
