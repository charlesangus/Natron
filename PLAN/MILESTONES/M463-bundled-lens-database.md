# M463 - Bundled Lens Database

Ship every photographic lens of the MIT-licensed OpticsDatabase (1,041 `.zmx` prescriptions across about 40 vendors, pinned to one commit) plus Niik-l's 9 built-ins as a "Classic Designs" group, bundled with the app and parsed by M462 - Shared Optics Library's `.zmx` parser — no network at runtime. Add a lens browser that organises them, and a shared lens-knob helper that LensFlare and M292 - Traced Lens Bokeh both use.

Sources: OpticsDatabase (`https://github.com/amegahed/OpticsDatabase`, MIT; credits Bill Claff/photonstophotos, Mikhail Polyanskiy/refractiveindex.info, Michael Hayford, Kats Ikeda/pencilofrays), `Optics/Photography/<Vendor>/*.zmx`. Realflare's `flare/adapters/database/database.py` shows the layout.

## Scope

**In:**
- A converter that strips the `.zmx` files to the lines M462's parser reads and writes one compressed archive committed in-repo (not LFS), plus the MIT notice and a per-lens source line.
- `Optics::LensDatabase`: lazy load, stable lens ids (`<vendor>/<file stem>`), metadata (vendor, name, focal length, f-number, element count, aspheric, anamorphic), and the Classic Designs group.
- Shared lens knobs: `lensModel` (each node lists its own choices; Database and From File always), `lens` (database id string) with a Browse… button, `lensFile`, `glassVendor`, `abbeOffset`, `useAspheric`.
- A lens browser dialog: vendor tree, search, filters (focal length range, f-number range, element count, aspheric, anamorphic), a details pane, and a live cross-section diagram of the selected lens with its paraxial facts. A preview-provider hook that M471 - Lens Browser Previews and M292 - Traced Lens Bokeh fill.

**Out:**
- Flare preview and pre-rendered thumbnails: M471 - Lens Browser Previews.
- Bokeh preview: M292 - Traced Lens Bokeh.
- Network download or update at runtime; user-editable database.

**Edge cases:**
- A saved `lens` id missing from the database (renamed upstream after a re-pin): a node error naming the id; the knob keeps the value.
- A database lens that fails to parse: excluded at conversion time with a converter warning, never at runtime.
- Search with no match: empty list, details cleared.

**Assumptions:**
- Unconfirmed: the stripped, compressed archive is under 2 MB (raw `.zmx` total 8.6 MB of mostly comments and unused fields).
- Unconfirmed: per-file provenance inside OpticsDatabase is covered by its top-level MIT licence; the converter keeps each file's source/`NOTE` line for attribution.

## Phase 463.1: Database

- [ ] M463.P1.T1 — Converter and bundled archive
  - files: `tools/optics/convert_lenses.py` (new), `Gui/Resources/Optics/lenses.zmx.gz` (new, generated, committed), `Gui/Resources/Optics/LICENSE-OpticsDatabase.txt`, `Gui/Resources/Optics/README.md` (new: pinned commit, how to re-run), `App/CMakeLists.txt` (M462's `Resources/optics` copy rule picks the archive up; add it to the stamp's dependencies)
  - approach: Arguments: a local OpticsDatabase checkout and its commit hash. For each `Optics/Photography/<Vendor>/*.zmx`, decode (UTF-16 or UTF-8), keep only the lines M462's parser reads plus `NOTE`, prefix a `#LENSID <vendor>/<stem>` header, concatenate and gzip. Write the commit hash into the archive header. Deterministic output (sorted, fixed gzip mtime).
  - verify: rerunning on the same checkout gives a byte-identical archive; archive size reported; every lens in it parses with M462's parser (the script calls a small parse check through the test binary or reports failures it excluded).
  - size: M

- [ ] M463.P1.T2 — LensDatabase loader
  - files: `Engine/Optics/LensDatabase.{h,cpp}` (new), `Engine/Optics/BuiltinLenses.cpp`, `Tests/Native/OpticsDatabase_Test.cpp` (new)
  - approach: Load the archive from the installed resources once, lazily and thread-safely; index lens ids and metadata (paraxial focal length and f-number from M462, element count, any aspheric surface, any cylinder → anamorphic). Add M462's 9 built-ins as vendor "Classic Designs". `prescription(id)` parses on demand and caches.
  - verify: gtest: more than 1,000 lenses load; ids are unique; Classic Designs has 9; `prescription()` of a Nikon lens matches parsing its original file from the fixture; concurrent first loads from 8 threads load once.
  - size: M

## Phase 463.2: Knobs and browser

- [ ] M463.P2.T1 — Shared lens knobs
  - files: `Engine/Optics/LensKnobs.{h,cpp}` (new, in the Engine library since it creates knobs), `Engine/Nodes/README.md`
  - approach: A helper that adds `lensModel` (the caller supplies extra leading choices such as Procedural), `lens` (string, default a caller-chosen id), a `lensBrowse` button ("Browse…"), `lensFile` (file, `.zmx`/`.dat`), `glassVendor` (choice: Schott, Ohara, Hoya, CDGM, Hikari, Sumita), `abbeOffset` (0, range ±20), `useAspheric` (on). Labels follow script names. `resolve(time)` returns the prescription or an error string. The browse button calls a GUI hook (`LensKnobs::setBrowseHandler`), so the engine has no GUI dependency.
  - verify: gtest with a test effect: switching `lensModel` to From File with a bad file gives the parse error; Database with an unknown id gives the missing-id error; a valid id returns its prescription.
  - size: M

- [ ] M463.P2.T2 — Lens browser dialog
  - files: `Gui/LensBrowser.{h,cpp}` (new), `Gui/CMakeLists.txt`, `Gui/GuiApplicationManager.cpp` (register the browse handler)
  - approach: A non-modal dialog: vendor tree on the left (Classic Designs first), a searchable, sortable table (name, focal length, f-number, elements, aspheric, anamorphic) with filters for focal length and f-number ranges, element count and the two flags; a details pane; a cross-section diagram drawn with QPainter from M462's `layout()` with a few traced axial and marginal rays. OK writes the id into the node's `lens` knob as one undoable edit. `LensBrowser::addPreviewProvider(name, fn)` adds a tab per provider, rendered on a worker thread and cancelled on selection change. Fits 1920x1080.
  - verify: `Tests/gui/m463_lens_browser.py` under Xvfb: open the browser from a test node, filter to 50 mm primes, select a lens, confirm, and check the knob value; screenshot the dialog with the diagram.
  - size: L

**Verification gate:** build and ctest green; the GUI script passes and its screenshots are shared with the user (collected at F30 - Lens Optics Base's UAT); the installed AppImage contains `Gui/Resources/Optics/` and the MIT notice; archive size recorded in Decisions.

## Decisions

- 2026-11-16 — **The full OpticsDatabase ships, bundled (user):** converted once and committed in-repo, no runtime network.
- 2026-11-16 — **A browser that organises the lenses, with live previews and pre-rendered thumbnails (user):** this milestone ships the browser and diagram; flare previews and thumbnails are M471 - Lens Browser Previews, the bokeh preview is M292 - Traced Lens Bokeh.
