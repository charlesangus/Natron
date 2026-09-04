# Ship a portable tarball and an AppImage

2026-09-03. Answers the question deferred on 2026-08-29
(`2026-08-29-defer-packaging-decision.md`): AppImage vs. Flatpak vs.
source-only. CI is green and the build is stable, so the choice is now
decidable.

**Two artifacts, one staged tree.** A relocatable `tar.xz` and an AppImage
built from it. Both carry all three binaries — `Natron`, `NatronRenderer`,
`natron-python` — the four OFX bundles, the OCIO configs, and the runtime
library closure. The tarball is the farm and shared-storage artifact; the
AppImage is the workstation artifact. AppImage is the wrong shape for a farm
(a FUSE mount per invocation, across thousands of frame jobs, on shared
storage), which is why the plain tarball is kept rather than treated as an
AppImage intermediate.

**Why not the alternatives.** *Flatpak* would mean rebuilding OCIO, OIIO,
PySide6, OpenFX and their transitive deps against `org.freedesktop.Platform`
— a second dependency build system running in parallel with the ASWF
container, guaranteed to drift from it, and awkward for a plugin host that
must see `/usr/OFX/Plugins` and arbitrary media paths. Revisit if someone
will own the manifest. *`.deb`/`.rpm`* built from a bundled tree is a tarball
with extra ceremony: it buys dependency *declaration* this fork does not need
(no distro ships these versions) and costs one build per distro. Reachable
later via CPack once Phase 14.1's install rules exist, if users ask.
*Distro-native* archives need deps that exist in no archive, a sponsor, and
would draw objections to the vendored `libs/` tree — a multi-year item, not
this one. *Qt Installer Framework*, which upstream used, is a build
dependency of its own with dated UX and `repogen` hosting to reproduce.

**No separate `NatronRenderer` container image.** It was considered and
rejected: the renderer is already installed beside the GUI by the same rules,
resolves resources through the same `applicationDirPath()` contract, and
shares all but six of its `DT_NEEDED` entries with the GUI binary. Splitting
it out buys a small size reduction and costs a second staging path, a second
artifact to test, and version skew between what artists preview and what the
farm renders. A four-line `FROM rockylinux:9` + `ADD` over the release
tarball remains available as a downstream convenience for k8s or cloud-burst
farms; it is not a parallel build.

**The glibc floor is EL9's 2.34**, set by building in
`aswf/ci-vfxall:2027-clang21.1`. That follows from `2026-08-29-target-vfx-cy2027.md`
and is accepted, not worked around: artifacts run on RHEL/Rocky 9+ and Ubuntu
22.04+, and not on Ubuntu 20.04. Building on an older base purely to widen the
floor would fork the toolchain the whole project is pinned to.
