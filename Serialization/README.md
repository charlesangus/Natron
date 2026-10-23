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
