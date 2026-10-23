# Serialization

`NatronSerialization` is a plain-data static library: the archive shape for
everything Natron saves, with no dependency on Engine or Gui classes.

## Rules

- Plain data only. A `*Serialization` type here holds built-in types,
  strings, containers and other `*Serialization` types -- never a
  `KnobIPtr`, `NodePtr`, or any other Engine/Gui pointer.
- No `Engine/`, `Gui/`, Qt or Python includes, anywhere under this directory.
  Conversion between plain data and live Engine/Gui objects
  (`toSerialization`/`fromSerialization`) lives in `Engine/` and `Gui/`, not
  here.
- No `boost/archive/...` or `boost/serialization/...` includes. Boost
  serialization is Engine's disk-cache table of contents only.
- yaml-cpp is a PRIVATE dependency: only `Serialization/Yaml/*.cpp` may include
  it, or `Serialization/Yaml/YamlPrivate.h`, which shares yaml-cpp helpers
  between them. The public `Serialization/Yaml/*.h` headers speak `std::string`.
- `tools/ci/check_serialization_layering.sh` enforces the include rules
  above and runs in the `lint-ci` CI job.

## Adding a field

1. Add the member to the plain-data struct in `Serialization/` (for example
   `NodeSerialization`), with a default that means "not saved".
2. Write it in the encoder under `Serialization/Yaml/` and read it back in the matching
   decoder, in the same file. Write the key only when the value is not the default, so
   existing documents stay byte-identical. Read the key through the `MapReader` so that it
   counts as known.
3. Fill and apply it in the Engine/Gui `toSerialization`/`fromSerialization` code.
4. Cover it in the round-trip test in `Tests/Serialization/` and, when it appears in
   `project.yaml`, in the pinned example in `ProjectYaml_Test.cpp`; then update
   `docs/project-format.md`.

A key that a reader does not know is reported as a warning and ignored, so an older Natron
still opens a file that carries your new key. For that reason `formatVersion` goes up only for
a change an older reader would misread: a renamed or removed key, or a changed meaning. Adding
an optional key does not bump it.
