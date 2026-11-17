# M478 - Cinema And Vintage Lens Prescriptions

Grow the bundled lens database beyond OpticsDatabase and Niik-l's 9 with cinema and vintage designs transcribed from patents and published sources (user, 2026-11-16). It must cover the reference product's whole library at least: Double-Gauss 50/2, Speed Panchro 50/2, Petzval 100/3.4, Helios-44 58/2, Aero-Ektar 178/2.5, Sonnar 50/1.5, Canon 50/1.2 (1956), Tessar 50/2.8, Trioplan-type triplet 100/2.8, wide 22/2.8, telephoto 250/5.6, Kino-Plasmat 100/2, Kapella 100/1.0, Tair-11 135/2.8, Jupiter-9 85/2, Heliar 100/4.5, Biotar 50/1.4, Orestor 135/2.8, Goerz 100/1.9 Primoplan type. Then it searches widely for cinema lenses: Zeiss Super Speed, Standard Speed, Ultra Prime, Master Prime and Supreme; Cooke Speed Panchro, S4, S5 and S7; Leica Summilux-C; Canon K35; Panavision Primo; and anamorphics (Cooke, Panavision C/E/G, Kowa, Hawk, LOMO, Atlas and others). Anamorphic designs need biconic and toroidal surfaces, which this milestone adds to M462 - Shared Optics Library.

Deps: M462 - Shared Optics Library, M463 - Bundled Lens Database.

## Scope

**In:**
- A researched candidate list with a source and a confidence for each lens, approved by the user before transcription (a `response` checkpoint).
- Biconic and toroidal surfaces in `Engine/Optics` (prescription, intersection, the `.zmx` `BICONICX` and `TOROIDAL` types), CPU and GPU (`opticsTrace.slang`).
- Transcribed prescriptions as `.zmx` with metadata lines: product, patent number and example, confidence (`confirmed` or `likely`), a source URL, blade count where known, and a one-line character note. M463's converter ingests them into the bundled archive.
- Naming (user, 2026-11-16): the product name with a "(patent)" tag, e.g. "Master Prime 50mm T1.3 (patent)", with the patent and example in the details. `likely` needs a cited source tying the patent to the product. The browser shows the tag, the confidence, the character note and the recommended blade count.
- Every design on the approved list, in batches of about ten.

**Out:**
- Auto-setting a node's blade count from the lens: the browser shows it, and the user sets `blades`.
- Zoom lenses (multi-configuration): M462's scope excludes them.
- Lenses whose only sources are paywalled or under a non-redistributable licence: listed in the research file as excluded, with the reason.

**Edge cases:**
- A lens already in OpticsDatabase: not duplicated, unless the patent version adds a distinct design (then both, with distinct names).
- A patent example whose paraxial EFL or f-number misses the patent's stated values by more than 2% (EFL) or 5% (f-number): a transcription error to fix, never shipped.
- A design that needs a surface type beyond biconic and toroidal (e.g. a freeform): excluded with a note.
- A glass given only by name: its nd and Vd come from the catalogue (M462), and the vendor goes in the metadata.

**Assumptions:**
- Unconfirmed: patent prescriptions are factual data that may be redistributed with a citation.
- Unconfirmed: most cinema primes have findable patent examples. Anamorphic front groups are mostly cylinders, with biconic or toroidal elements in some designs.

## Phase 478.1: Research and format

- [ ] M478.P1.T1 — Research the candidate list
  - files: `tools/optics/patent-lenses/CANDIDATES.md` (new)
  - approach: For every lens in the intro, and every other cinema or vintage design found, search Google Patents and Espacenet (US, DE, GB, JP, FR, SU), the classic texts (Kingslake's "A History of the Photographic Lens", Smith's "Modern Lens Design", Laikin's "Lens Design") and lens-history sites. Check the pinned OpticsDatabase commit for existing copies. One row per lens: product, patent and example, confidence with its supporting source, focal length, f-number, surface types needed, source URLs and notes. Never name other products' lens libraries. When done, the PM sets the row's `Needs` to `response` for the user's approval of the list.
  - verify: the file lists all 19 intro designs (or why one is excluded) and at least the Zeiss and Cooke cine families and five anamorphic designs; every row has a source and a confidence; OpticsDatabase duplicates are flagged.
  - size: M

- [ ] M478.P1.T2 — Biconic and toroidal surfaces
  - files: `Engine/Optics/Prescription.{h,cpp}`, `Engine/Optics/Trace.{h,cpp}`, `Engine/Optics/ZemaxParser.{h,cpp}`, `Engine/Gpu/Kernels/opticsTrace.slang`, `Tests/Native/OpticsTrace_Test.cpp`, `Tests/Native/OpticsZemax_Test.cpp`, `Tests/fixtures/optics/biconic.zmx` (new, small)
  - approach: Add Biconic (separate x and y curvature and conic) and Toroidal (a rotated circle's radius plus a y-polynomial) surface types, with Newton intersection like the asphere's (12 steps, a miss on no convergence). Parse `BICONICX` and `TOROIDAL` with their `PARM` layout. Mirror the intersection in the Slang include.
  - verify: gtest: a biconic with equal curvatures matches a sphere's intersections within 1e-9; a biconic with zero y-curvature matches Cylinder X; the fixture parses; CPU and GPU intersections agree within 1e-6.
  - size: L

- [ ] M478.P1.T3 — Patent lens metadata through the converter, database and browser
  - files: `tools/optics/convert_lenses.py`, `tools/optics/patent-lenses/README.md` (new: the `.zmx` header format), `Engine/Optics/LensDatabase.{h,cpp}`, `Gui/LensBrowser.{h,cpp}`, `Tests/Native/OpticsDatabase_Test.cpp`
  - approach: Patent lenses are `.zmx` files under `tools/optics/patent-lenses/<vendor>/`, with `#META key: value` header lines (product, patent, example, confidence, source, blades, character). The converter adds them to the archive after OpticsDatabase, keeping the meta lines. `LensDatabase` exposes them as metadata. The browser adds a "(patent)" tag to names, plus confidence, character and recommended blades in the details, and a filter for patent designs.
  - verify: gtest with two fixture patent lenses: the metadata loads; ids are `<vendor>/<stem>`; the converter output is still byte-identical on rerun; `Tests/gui/m463_lens_browser.py` still passes.
  - size: M

## Phase 478.2: Transcription

Each batch transcribes about ten approved designs into `tools/optics/patent-lenses/<vendor>/*.zmx` from the approved list's sources, then regenerates the archive. Each batch's verify is the same: every new lens parses; its paraxial EFL is within 2% and its f-number within 5% of the source's stated values (`Tests/Native/OpticsPatentLenses_Test.cpp`, new in batch 1, reads the expected values from `#META efl:`/`fnum:` lines); it focuses at 5 m and bakes a bokeh table (M292). If the approved list runs past four batches, further batches are added as trailing tasks.

- [ ] M478.P2.T1 — Batch 1: the vintage designs
  - files: `tools/optics/patent-lenses/` (new `.zmx` files), `Gui/Resources/Optics/lenses.zmx.gz`, `Tests/Native/OpticsPatentLenses_Test.cpp` (new)
  - approach: Every intro design not already in the database (Speed Panchro, Helios-44, Biotar, Aero-Ektar, Canon 50/1.2, the Trioplan-type triplet, Kino-Plasmat, Kapella, Tair-11, Jupiter-9, Heliar, Orestor, Goerz Primoplan type, and any of the generic designs missing), using the list's sources. Set the Tair-11's blades to 20 and the Orestor's to 15.
  - verify: as the phase intro says.
  - size: M

- [ ] M478.P2.T2 — Batch 2: Zeiss cine primes
  - files: `tools/optics/patent-lenses/Zeiss/`, `Gui/Resources/Optics/lenses.zmx.gz`, `Tests/Native/OpticsPatentLenses_Test.cpp`
  - approach: The approved Super Speed, Standard Speed, Ultra Prime, Master Prime and Supreme examples.
  - verify: as the phase intro says.
  - size: M

- [ ] M478.P2.T3 — Batch 3: Cooke and other spherical cine primes
  - files: `tools/optics/patent-lenses/` (Cooke, Leica, Canon, Panavision and others), `Gui/Resources/Optics/lenses.zmx.gz`, `Tests/Native/OpticsPatentLenses_Test.cpp`
  - approach: The approved Cooke (Speed Panchro, S4, S5, S7), Summilux-C, K35, Primo and other spherical cine examples.
  - verify: as the phase intro says.
  - size: M

- [ ] M478.P2.T4 — Batch 4: anamorphics
  - files: `tools/optics/patent-lenses/` (anamorphic vendors), `Gui/Resources/Optics/lenses.zmx.gz`, `Tests/Native/OpticsPatentLenses_Test.cpp`
  - approach: The approved anamorphic examples, using cylinder, biconic and toroidal surfaces. Record the squeeze in `#META squeeze:` and check it against `Paraxial`'s per-meridian focal lengths.
  - verify: as the phase intro says, plus each lens's paraxial squeeze within 2% of its `#META squeeze:`.
  - size: M

## Phase 478.3: Docs

- [ ] M478.P3.T1 — Lens list docs and browser screenshots
  - files: `Gui/Resources/Optics/README.md`, `Engine/Nodes/Filter/docs/ZDefocus.md` (the lens section), `Tests/gui/m478_patent_lenses.py` (new)
  - approach: Document every patent lens with its citation, confidence and character, and how to add one. Never name other products. The GUI script filters the browser to patent designs, opens a vintage, a Zeiss and an anamorphic lens, and screenshots each.
  - verify: the GUI script passes, and its screenshots are collected for F13 - ZDefocus Rework's UAT; `ctest -R Optics` passes.
  - size: M

**Verification gate:** the full build and `ctest -R Optics` pass, with biconic/toroidal GPU parity on RADV; every approved lens is in the archive and passes the EFL, f-number and focus checks; the user approved the candidate list (recorded in `## Decisions`); the GUI screenshots are collected at F13 - ZDefocus Rework's UAT.

## Decisions

- 2026-11-16 — **Every missing vintage design plus a wide cinema search (user, /cat-plan grill):** "also search for any lenses that we can find that aren't in the database or that list, focusing especially on cinema lenses, Zeiss super speed, standard, ultra and Master prime, cooke s, anamorphics, etc".
- 2026-11-16 — **Product name plus a "(patent)" tag, with a confidence (user).**
- 2026-11-16 — **Research all, transcribe in batches, after the user approves the list (user).**
