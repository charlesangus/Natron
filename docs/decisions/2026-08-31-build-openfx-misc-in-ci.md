# openfx-misc is built from source in CI

`openfx-misc` is built from pinned source in `tools/ci/local/fetch-assets.sh`,
the same way `openfx-io` already is, against the image's own gcc 14.2 / C++20 /
OpenColorIO 2.5.2 / OpenEXR 3.4.15.

Nothing in the current test surface requires it: `BaseTest`'s three registered
plugin IDs (`ReadOIIO`, `WriteOIIO`, `SeNoise`) are satisfied by `openfx-io`
plus `SeExpr` alone, and `SeNoise` lives in `openfx-io` — not in `openfx-misc`,
contrary to what an earlier decision claimed while arguing to drop the vendored
plugin tests.

It is built anyway because "does this fork's toolchain compile the plugin set
Natron actually ships with?" is a question worth having answered continuously
rather than discovering at release time. The cost is build time on every CI
run, accepted deliberately.

Supersedes the `openfx-misc` half of the original `M3.P1.T4`, whose framing —
"confirm they build cleanly before wiring them into the new CI" — assumed CI
was still ahead of it. CI shipped first.
