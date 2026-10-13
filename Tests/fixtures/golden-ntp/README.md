# Golden `.ntp` fixtures

This directory holds the committed, byte-for-byte golden files that
`Tests/Serialization/GoldenXml_Test.cpp` checks every save against. Each case
(a real fixture reloaded and resaved, or a graph built from scratch in the
test) is saved to a temporary directory, then normalized — the fields that
legitimately vary from run to run (the Natron version/git/OS/bits header, the
project `CreationDate`, the `lastSaveDate`/`lastAuthor`/`originalAuthor`/
`softwareVersion` info-page knobs, and each node's `KnobsAge`/`CacheID`) are
replaced with a fixed placeholder — and the result is compared against
`<case-name>.ntp` here.

## Regenerating

From the repository root, with a built `build/debug` tree and `build/assets` fetched:

```sh
tools/ci/local/devshell.sh bash -c 'cd build/debug/Tests && \
  NATRON_REGEN_GOLDEN=1 OFX_PLUGIN_PATH=$PWD/../../assets/Plugins \
  xvfb-run --auto-servernum ./Tests --gtest_filter="GoldenXmlTest.*"'
```

Every case writes or overwrites its own `<case-name>.ntp` here and passes
unconditionally while `NATRON_REGEN_GOLDEN` is set.

Read the diff before committing: a golden should only move because a
serialization change was intentional. Then re-run the same command without
`NATRON_REGEN_GOLDEN` to confirm every case compares equal.
