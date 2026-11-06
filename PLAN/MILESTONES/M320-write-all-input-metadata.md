# M320 - Write All Input Metadata

The native Write drops most of its input's metadata. `OiioWriteSupport::applyMetadata()`
(`Engine/Nodes/IO/OiioWriteSupport.cpp`, ~:938-1000 on main as of 2026-11-03) writes only
`exr/`, `exif/`, `tiff/`, `dpx/`, `cin/` and `oiio/` keys, each only for its own format,
plus `ofx/timecode` and `ofx/edgecode`. Every other key — unprefixed user keys from
AddMetaData/ModifyMetadata, `ofx/creator`, `ofx/comment`, `ofx/slateinfo`, `ofx/framerate`,
the camera `ofx/` keys, `quicktime/` keys, and `exr/` keys bound for a non-EXR file — hits a
silent `else { continue; }`. This milestone makes Write put everything the file format can
hold into the file, behind a Nuke-style `metadata` choice knob, and makes the Read map the
new attribute names back.

## Scope

**In:**
- A write mode on `applyMetadata()`: Default, All Metadata Except Input, All Metadata (No
  Metadata is the caller skipping the call).
- Default: today's format-native routing plus the standard `ofx/` keys mapped to standard
  tags.
- All Metadata Except Input: Default plus every remaining key under its full key name, in
  formats that take arbitrary attributes (EXR; PNG as text chunks).
- All Metadata: also the Read's file-identity keys (`ofx/filepath`, `ofx/frame`,
  `ofx/filesize`, `ofx/mtime`, and any other identity key the native Read adds).
- A `metadata` choice knob replacing the `writeMetadata` bool, defaulting to All Metadata
  Except Input.
- Read side: an attribute whose name already is a Natron metadata key (`<namespace>/<name>`)
  comes back as that key verbatim.

**Out:**
- Custom keys in TIFF, JPEG, DPX, Cineon and other formats without arbitrary attributes
  (no XMP packing); they get their standard tags only.
- Merging metadata from several inputs; Write keeps using its first non-mask input.
- Exact integer-width / string-array round-trip (uint8, int64, string lists joined by the
  Read).
- Importing old projects' `writeMetadata` value (no backward compat).

**Edge cases:**
- A key whose verbatim name is a structural attribute (`compression`, `name`, `type`,
  `view`, `PixelAspectRatio`, `ColorSpace`, …) is never written in any mode — it would change
  how the file is encoded. The existing `describesSourceLayout` / `isExcludedOiioName` lists
  apply to verbatim names too.
- An unprefixed user key (`shot`) written to EXR reads back as `exr/shot`; accepted.
- `natron:outputHash` is still added for EXR, after the metadata, and is never overwritten
  by an input key of the same name.
- A Default-mode `ofx/` mapping never overrides a format-native key already present for the
  same tag (e.g. `exif/Software` wins over `ofx/creator`).

## Phase 320.1: Write Modes

- [x] M320.P1.T1 — Add a write mode to `applyMetadata()` and map the standard `ofx/` keys in Default mode
  - files: Engine/Nodes/IO/OiioWriteSupport.h, Engine/Nodes/IO/OiioWriteSupport.cpp, Tests/Native/NativeWriteMetadata_Test.cpp
  - approach: Add `enum class WriteMetadataMode { Default, AllExceptInput, All }` in `OiioWriteSupport.h` and a mode parameter on `applyMetadata()`; update the one caller in `NativeWrite.cpp` (~:2215) to pass `AllExceptInput` for now. In Default mode keep today's routing and add standard-tag mappings for `ofx/` keys (names in `libs/OpenFX/include/ofxMetadata.h`): creator → `Software`, copyright → `Copyright`, comment → `ImageDescription`, framerate → `FramesPerSecond` (RATIONAL, as `setValue` already does), focallength/fnumber/exposuretime → `Exif:FocalLength`/`Exif:FNumber`/`Exif:ExposureTime` (unprefixed names for non-EXR where `isUnprefixedExifTag` says so). A format-native key for the same tag wins. AllExceptInput and All behave as Default in this task. Update the header comment at ~:199-205. Extend `OiioWriteMetadata.MapsEachPrefixToItsAttributeAndFormat` (or add a sibling case) on a bare `ImageSpec`.
  - verify: the extended `OiioWriteMetadata` unit tests pass (build done by the PM); existing NativeWriteMetadata tests still pass.
  - size: M
- [ ] M320.P1.T2 — Write every remaining key verbatim in All Metadata Except Input and All Metadata
  - files: Engine/Nodes/IO/OiioWriteSupport.cpp, Engine/Nodes/IO/OiioWriteSupport.h, Tests/Native/NativeWriteMetadata_Test.cpp
  - approach: Replace the silent `else { continue; }` with: in AllExceptInput/All, write any key Default did not consume under its full key name (`ofx/creator`, `exif/Make`, `shot`, `exr/X` on PNG as `exr/X`) when the format takes arbitrary attributes — openexr, and png for string values (check OIIO's `pngoutput.cpp` in the natron-dev image for which attribute types become text chunks; stringify non-string values for PNG). Formats without arbitrary attributes keep the existing `qDebug` "not written" line. Define the input-identity key set from what `OiioReadSupport.cpp` / the native Read add (`ofx/filepath`, `ofx/frame`, `ofx/filesize`, `ofx/mtime`, …) — skipped in AllExceptInput, written verbatim in All. Structural names stay excluded in every mode (see Scope). For EXR keep doubles as DOUBLE instead of narrowing to float. Add unit cases on a bare `ImageSpec` for each mode and for EXR vs PNG vs TIFF.
  - verify: new `OiioWriteMetadata` unit cases pass per mode/format; structural-name and identity-key cases behave as in Scope.
  - size: M

## Phase 320.2: Knob And Read-Back

- [ ] M320.P2.T1 — Replace the `writeMetadata` bool with a `metadata` choice knob
  - files: Engine/Nodes/IO/NativeWrite.h, Engine/Nodes/IO/NativeWrite.cpp, Engine/Nodes/IO/docs/NativeWrite.md, Engine/Nodes/README.md, Tests/Native/NativeWriteMetadata_Test.cpp
  - approach: Replace `kNatronWriteParamWriteMetadata`/`KnobBool` (NativeWrite.cpp ~:1022-1028, member at NativeWrite.h ~:335) with a `KnobChoice` script name `metadata`, label "Metadata", options No Metadata / Default Metadata / All Metadata Except Input / All Metadata, default All Metadata Except Input, animation off; tooltip says what each option writes and that formats without custom attributes (TIFF, JPEG, DPX) take only their standard tags. At ~:2213 skip `applyMetadata` for No Metadata, else pass the mapped mode. Grep `.local/docs` (Nuke Write `metadata`) for option wording. Update NativeWrite.md and the README metadata routing section (~:273). Rewrite `SwitchedOffWritesNoneOfTheCustomAttributes` and `WriteMetadataDefaultsOn` for the new knob; grep Tests/ and Tests/gui for other `writeMetadata` users and update them.
  - verify: NativeWriteMetadata tests pass with the new knob; `git grep writeMetadata` finds no stale users.
  - size: M
- [ ] M320.P2.T2 — Read namespaced attribute names back as their key
  - files: Engine/Nodes/IO/OiioReadSupport.cpp, Tests/Native/OiioReadSupport_Test.cpp
  - approach: In `keyOf` (~:696-720), before the unprefixed fallback, map an attribute name of the form `<ns>/<rest>` where `<ns>` is a metadata namespace from `ofxMetadata.h` (`ofx`, `exr`, `exif`, `tiff`, `dpx`, `cin`, `oiio`, `quicktime`) to the key verbatim, for every format. Other names keep today's mapping. Add `OiioReadSupport_Test` cases on a bare `ImageSpec` with `ofx/creator`, `exif/Make` (in an EXR and a PNG spec) and an unprefixed name.
  - verify: the new OiioReadSupport unit cases pass; existing NativeReadMetadata tests still pass.
  - size: M

## Phase 320.3: End-To-End

- [ ] M320.P3.T1 — Round-trip tests through Read → metadata nodes → Write
  - files: Tests/Native/NativeWriteMetadata_Test.cpp
  - approach: Extend the Read → Dot → Write fixture with a ModifyMetadata (or AddMetaData) node setting an unprefixed user key, `ofx/comment`, `ofx/creator` and a double. For EXR and PNG, in each mode, write, read back with the native Read and with OIIO directly, and assert: Default has `ImageDescription`/`Software` but no `shot`; All Metadata Except Input round-trips every key (EXR double exact) and has no `ofx/filepath`; All Metadata has `ofx/filepath`; No Metadata writes none; structural names never appear. Keep fixtures synthetic and tiny (LFS quota).
  - verify: the new NativeWriteMetadata cases pass in the build's ctest.
  - size: M

**Verification gate:** one build + full ctest green (PR CI debug asserts included); a release AppImage plus a UAT steps .md where the user renders a Read → ModifyMetadata → Write to EXR and PNG in each `metadata` mode and inspects the files with `oiiotool --info -v`; an Xvfb screenshot of the Write panel's `metadata` knob shared before sign-off.

## Decisions

- 2026-11-03 — Write everything the format can hold (user): keys go in under their full key name in EXR and PNG; only file-identity and structural keys stay out by default.
- 2026-11-03 — Nuke-style `metadata` choice knob replaces the `writeMetadata` bool (user); default All Metadata Except Input (planner, so the reported loss is fixed out of the box).
- 2026-11-03 — Foreign keys in EXR use the full key as attribute name (user), and the Read maps such names back verbatim.
