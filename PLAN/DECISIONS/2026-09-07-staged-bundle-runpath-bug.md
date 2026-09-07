# Staged release bundles are not actually relocatable

2026-09-07. `tools/release/stage-bundle.sh` sets `$ORIGIN/../lib` as the RUNPATH on
the three `bin/` executables but never sets one on the libraries it copies into
`lib/` during its ldd-closure walk — 62 of 82 bundled `.so` files carry no
RPATH or RUNPATH at all.

`DT_RUNPATH` is not transitive: a library's RUNPATH applies only to its own direct
`DT_NEEDED` entries, not to anything further down the chain. So a bundled library
depending on another bundled library beside it does not resolve. Concretely,
`libOpenColorIO.so.2.5` needs `libImath-3_2.so.30`, both staged into the same
`lib/`, and the bundle fails at startup with "cannot open shared object file".

This is invisible in the dev container, which has the ASWF VFX libraries installed
system-wide so `ld.so.cache` satisfies the lookup. It would fail on essentially any
ordinary Linux desktop. M15's verification exercised packaging only inside that
container, so the gate passed on a bundle that is not in fact relocatable — the
thing the milestone existed to produce.

Found while building an AppImage for M17's manual GUI check, where it was worked
around by running `patchelf --set-rpath '$ORIGIN'` over the staged libraries in the
gitignored build tree. The script is unchanged and still wrong.

Fix: set `$ORIGIN` (or `$ORIGIN/../lib` as appropriate) on every library staged into
`lib/`, not just on the executables, and verify a bundle on a machine without the
VFX libraries installed — the container cannot detect this class of bug.
