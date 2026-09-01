# CI runs on the OCIO config Natron resolves for itself

`tools/ci/local/test.sh` used to point `OCIO` at
`build/assets/OpenColorIO-Configs/blender/config.ocio` from the fetched tarball.
It now **unsets** `OCIO` for both the `ctest` and `smoke` targets, so what the
tests exercise is the config `Settings::tryLoadOpenColorIOConfig()` resolves —
the ACES 2.0 Studio built-in that users actually get — rather than an on-disk
config no default install has.

The blocker that previously justified pinning the tarball is gone: it was
`openfx-io` manufacturing the colorspace name `default`
(`2026-09-01-fix-openfx-io-colorspace-sentinel.md`), fixed on our fork and
pinned. Nothing else depended on the tarball — `Tests/` never referenced `OCIO`,
and only the smoke render consumed it — so the file's presence was also dropped
from `test.sh`'s asset precondition. `fetch-assets.sh` still fetches the configs;
they remain the fixture for testing the on-disk override path.

`unset` rather than merely not setting it: an `OCIO` inherited from a
developer's shell would silently substitute a different config for the one under
test.

**The catch this exposed, and why the smoke test grew a second assertion.** With
`OCIO` unset, OpenColorIO does not fail — it falls back to `ocio://default`, the
ACES 2.0 *cg* config, which encodes sRGB just as correctly and lands scene-linear
0.18 on the same 118/255. So the colour assertion alone cannot distinguish
"Natron resolved its default" from "Natron's default resolution broke and OCIO
papered over it". `smoke_test.py` now asserts the *name* of the active config via
`PyOpenColorIO.Config.CreateFromEnv()` — not `os.environ`, which cannot answer
it, because CPython snapshots the environment before Natron's `qputenv()`.

Generalises past OCIO: **when CI stops setting a variable so that the product
picks the value itself, check which value it picked.** A fallback that happens to
be correct will otherwise hide a broken resolution path indefinitely.
