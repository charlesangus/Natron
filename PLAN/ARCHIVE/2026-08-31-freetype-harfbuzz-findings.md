# FreeType/HarfBuzz Link Failure Findings

## Summary

`NatronRenderer` fails to link in `aswf/ci-baseqt:2027.0` because the final
link does not reliably resolve HarfBuzz's FreeType dependencies.  This is not
evidence that ASWF's bundled FreeType lacks COLRv1 support.

The recommended fix is to link HarfBuzz and FreeType explicitly, in that
order, through ASWF's CMake package targets.  Moving the build to the 2026
ASWF image is not expected to fix this issue.

## Observed CI Evidence

GitHub Actions run `33306839778` configured the project with:

```
-- Found Freetype: /usr/local/lib/libfreetype.so (found version "2.14.3")
```

The same configuration emitted CMake's runtime-search-path warning that
`/usr/local/lib/libfreetype.so.6` conflicts with the operating-system copy in
`/usr/lib64`.  `NatronRenderer` subsequently failed to link with unresolved
references from `/usr/local/lib/libharfbuzz.so.0`:

```
FT_Get_Color_Glyph_Paint
FT_Get_Paint
FT_Get_Paint_Layers
FT_Get_Transform
FT_Get_Color_Glyph_ClipBox
FT_Get_Colorline_Stops
```

`FT_Get_Color_Glyph_Paint` and the COLRv1 API family were introduced in
FreeType 2.13.  The FreeType 2.14.3 library CMake found is therefore new
enough.  The unresolved references instead show that the final linker command
is not using that library to satisfy HarfBuzz's symbols, or is seeing the
libraries in an order that prevents it from doing so.

## Why the Existing Change Is Insufficient

`Engine/CMakeLists.txt` currently finds `Freetype` and exposes
`Freetype::Freetype` through the static `NatronEngine` target.  This did not
change the failure in the CI run above.

`PkgConfig::Cairo` is not an adequate way to express the HarfBuzz-to-FreeType
relationship: normal dynamic `pkg-config --libs` metadata does not necessarily
surface those transitive dependencies.  The final executable must have an
explicit, ordered HarfBuzz and FreeType link relationship.

## Recommended CMake Change

In `Engine/CMakeLists.txt`, use ASWF's HarfBuzz CMake package and list it
directly before FreeType in `NatronEngine`'s public link interface:

```cmake
find_package(harfbuzz CONFIG REQUIRED)
find_package(Freetype REQUIRED)

target_link_libraries(NatronEngine
    PUBLIC
        HostSupport
        Boost::headers
        Boost::serialization
        PkgConfig::Cairo
        harfbuzz::harfbuzz
        Freetype::Freetype
    # existing private dependencies
)
```

ASWF's HarfBuzz Conan recipe declares FreeType as a dependency and retains its
CMake package files.  This uses the container's intended dependency metadata
and makes the required ordering explicit: HarfBuzz first, then FreeType.

Verify the change with a verbose CI link command.  The `NatronRenderer` link
line must show the ASWF `/usr/local` HarfBuzz library before the matching ASWF
FreeType library.  Also inspect the resulting binary's `DT_NEEDED` entries and
runtime search path so it cannot accidentally load `/usr/lib64` FreeType.

## Would ASWF 2026 Help?

No, not as a fix.

The released `aswf/ci-baseqt:2026.5` configuration uses FreeType 2.13.2 and
HarfBuzz 11.0.1.  FreeType 2.13.2 already provides the COLRv1 symbols at
issue.  It is based on Rocky Linux 8 and still has an older operating-system
FreeType under `/usr/lib64` alongside ASWF's `/usr/local` copy, so it preserves
the collision/link-resolution condition.

`aswf/ci-baseqt:2027.0` uses FreeType 2.14.3 and HarfBuzz 14.2.1.  Downgrading
to 2026 neither supplies a newer required API nor removes the dual-library
layout.

## Sources Examined

- Natron GitHub Actions run `33306839778` failure log.
- `Engine/CMakeLists.txt` in this checkout.
- ASWF Docker release metadata and `versions.yaml` for `ci-baseqt:2026.5` and
  `ci-baseqt:2027.0`.
- ASWF's vendored HarfBuzz and FreeType Conan recipes.
- FreeType API documentation for the COLRv1 API introduction version.
