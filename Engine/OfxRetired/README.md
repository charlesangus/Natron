# Retired OFX plug-ins

Each file in this directory, other than this README, retires one OFX plug-in: Natron no longer
registers it, although the bundle that provides it is still shipped. A native node replaces it.

## Format

- The file name is the OFX plug-in ID, with no extension, e.g. `net.sf.openfx.SharpenPlugin`.
- The content is empty, or a single line holding the ID of the native plug-in that replaces it,
  e.g. `CornerPin`. Native IDs are plain; a dotted one is accepted too.

One file per ID, so ports landing in parallel never edit the same file. Add the markers for the
IDs a port retires in that port's change.

## How it is used

`Engine/CMakeLists.txt` reads the markers at configure time and fails on a file name that is not
a dotted OFX plug-in ID, on a line that is not a plug-in ID, or on a marker of more than one line. It compiles them, sorted by ID,
into `OfxRetiredAll.inc`, which `Engine/OfxRetired.cpp` exposes as `retiredOfxPlugins()`.
`OfxHost::loadOFXPlugins()` skips every listed ID. A project or script that asks for a retired ID
gets the missing-plug-in node, like any absent plug-in.

## Checks

- `OfxRetired_Test` fails when a marker names an ID that no loaded bundle provides (a typo).
- `NativePluginList_Test` fails when a marker names a replacement that is not a registered
  native plug-in.
- `NativePluginList_Test` also fails when a retired ID is itself a native plug-in ID. Many
  native nodes kept the ID of the OFX plug-in they replaced (`net.sf.openfx.GradePlugin`, for
  one). The native registration already takes that ID's place, and `NativePluginList_Test`
  checks that no OFX version of it is left, so a marker for it would hide nothing. Only an OFX
  ID that no native node took over needs a marker.
