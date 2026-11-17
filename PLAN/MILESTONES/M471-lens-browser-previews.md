# M471 - Lens Browser Previews

Fill M463 - Bundled Lens Database's lens browser with flare samples: a live flare preview of the selected lens at the calling node's settings, and a pre-rendered flare thumbnail for every database lens, generated at package time and shipped with the app, shown in a thumbnail grid view.

## Scope

**In:**
- A flare preview provider for the browser: low-quality LensFlare render (GPU when available) of the selected lens with the calling node's lights, f-stop and coatings, or a default light when opened from a non-flare node.
- A package-time thumbnail step: every database lens rendered at a fixed preset (one off-axis light, D65, 3 wavelengths, 32 max divisions, starburst on) to 128 × 72 PNGs, packed into one archive installed under `Resources/optics/`, skipped when the database archive and the renderer version are unchanged.
- A grid view in the browser showing thumbnails, switchable with the table.

**Out:**
- Bokeh preview: M292 - Traced Lens Bokeh.
- Committing thumbnails to the repo: generated at package time only (user, 2026-11-16).

**Edge cases:**
- Thumbnail archive missing (a dev build that skipped the step): the grid shows the lens diagram instead and the table works.
- A lens whose flare render fails at package time: a placeholder thumbnail and a packaging warning, never a failed package.
- Selection changes while a live preview renders: the render is cancelled and restarted.

**Assumptions:**
- Unconfirmed: the CPU path renders all 1,041 thumbnails in under 15 minutes at the preset on the packaging runner; if not, the step uses the GPU path where available or a lower preset, and the time is recorded.

## Phase 471.1: Previews

- [ ] M471.P1.T1 — Live flare preview
  - files: `Gui/LensBrowser.{h,cpp}`, `Engine/Nodes/Generator/LensFlare.{h,cpp}` (a static preview-render entry), `Tests/gui/m471_lens_browser_previews.py` (new)
  - approach: Register a "Flare" preview provider: build a transient LensFlare render from the calling node's knobs (or defaults) with the selected lens, at 480 × 270, `wavelengths` 3, `maxDivisions` 32, on a worker thread with abort on selection change.
  - verify: the GUI script opens the browser from a LensFlare node, selects two lenses and screenshots two differing previews; changing the selection mid-render does not block the UI (the script's event loop timing stays under 100 ms per step).
  - size: M

- [ ] M471.P1.T2 — Package-time thumbnails
  - files: `tools/optics/render_thumbnails.py` (new, a NatronRenderer script), `tools/ci/local/package.sh` (or the AppImage packaging script it calls), `App/CMakeLists.txt` (install the archive when present)
  - approach: The script loads the database, renders each lens at the preset with LensFlare (CPU or GPU as available) through NatronRenderer, writes 128 × 72 PNGs and packs them with an index into `thumbnails.zip` keyed by lens id plus a header with the database archive hash and renderer version. Packaging runs it when that header differs from the cached one, keeping the cache in the build directory.
  - verify: running the packaging step twice renders once (second run skips, logged); the AppImage contains the archive; a lens that fails gets a placeholder and a warning; total time logged.
  - size: M

- [ ] M471.P1.T3 — Thumbnail grid in the browser
  - files: `Gui/LensBrowser.{h,cpp}`, `Tests/gui/m471_lens_browser_previews.py`
  - approach: A table/grid toggle; the grid shows thumbnails from the installed archive (lazy decode), honouring the same filters and search; with no archive it shows diagrams.
  - verify: the GUI script switches to the grid on an AppImage with the archive, filters to fisheyes and screenshots the grid.
  - size: M

**Verification gate:** build, ctest and the GUI script green on an AppImage built with `package.sh`; grid and preview screenshots shared with the user (collected at F31 - Lens Flares' UAT); thumbnail generation time recorded in Decisions.

## Decisions

- 2026-11-16 — **Live preview on selection and pre-rendered thumbnails, rendered at build/package time (user, /cat-plan grill).**
