# M634 - VDB Grids In Geometry Cache

M403 - Geometry File Cache (in F18 - Geometry Loops And Solver, landed) does not serialize `GeoDetail::sharedGrids()`. M417 - VDB Volume Primitive (in F22 - Native VDB) makes VDB primitives valid. F22 lands second, so it must add grid stream I/O to the GeoFileFormat writer and reader. Without it, a cached detail with VDB primitives reloads with a dangling `vdbGrid` index and throws "VDB primitive has no grid". On F22, `_sharedGrids` is already hashed into the content hash (`Engine/Geo/GeoDetail.cpp:445-446`), but `Engine/Geo/GeoFileFormat.cpp` is identical to main. The format is at `kGeoFileFormatVersion = 2` (`GeoFileFormat.h:63`).

## Scope

**In:**
- Writing and reading shared VDB grids in GeoFileFormat, a format version bump, and a round-trip test.

**Out:**
- Tying the separate `<disk cache>/vdb/<hash>.vdb` field files into cache eviction (an open question on M417).

**Edge cases:**
- **Old version-2 files:** no backward compatibility; the reader rejects them with a clear message, as for any other version mismatch.
- **Grid shared by several primitives:** written once, and the indices still resolve after load.
- **Memory-mapped read:** grids are decoded from the mapped bytes through an OpenVDB stream; partial mapping is out.

## Phase 634.1: Grid I/O

- [ ] M634.P1.T1 — Serialize shared grids in the GeoFileFormat writer and reader
  - files: `Engine/Geo/GeoFileFormat.{h,cpp}` (writer, reader, `kGeoFileFormatVersion`), `Engine/Geo/GeoDetail.{h,cpp}` (`sharedGrids`); `Tests/GeoFileFormat_Test.cpp` (near :219 and :275)
  - approach: bump the version to 3. After the attribute sections, write a grid section: count, then per grid its byte length and an `openvdb::io::Stream` blob. On read, decode the grids into the detail's shared grid list before primitives resolve `vdbGrid`. Add round-trip tests for a fog volume and an SDF (one grid shared by two prims), comparing voxel values and metadata.
  - verify: the round-trip tests pass; loading a cached VDB detail no longer throws; `ctest -R "GeoFileFormat|Vdb"` green
  - size: M

**Verification gate:** full build and ctest green on the F22 branch; a GeoFileCache write-then-load of a VDB detail renders the same volume in a test.
