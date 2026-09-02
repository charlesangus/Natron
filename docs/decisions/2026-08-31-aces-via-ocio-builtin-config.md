# ACES 2.0 comes from OpenColorIO's built-in config, and old projects break loudly

Natron's default OCIO config moves to ACES 2.0 via OpenColorIO's **built-in
configs** — addressable as `ocio://` URIs — rather than by shipping or
downloading a config tarball. OCIO 2.5.2 is already in the CI image, so the
config becomes a property of the library the build links against instead of a
2018-era archive fetched over the network and installed next to the binary.

This also dissolves a problem the plan had not noticed: nothing in the
CMake-only build ever installs an `OpenColorIO-Configs` directory where
`Engine/Settings.cpp`'s `getDefaultOcioConfigPaths()` looks for one. The only
code that ever did is a qmake/Jenkins-era installer script no workflow invokes.
With `ocio://` there is no directory to install, so the gap closes by
construction rather than by building new packaging.

**Existing projects are an explicit, documented break.** Colorspaces are stored
in `.ntp`/`.ntf` as bare strings (`"Linear"`, `"sRGB"`, `"rec709"`) and matched
by name against the active config; ACES configs name them differently
(`ACES2065-1`, `ACEScg`, `sRGB - Display`), so a project saved against the old
default cannot resolve them. Rather than a name-mapping table (whose per-name
choices are guesses that fail silently) or pinning each project's config at save
time (a project-format change), such a project must **fail loudly with an
actionable message** telling the user to re-select colorspaces.

The rejected options are worth naming because they are the right answers for a
different project: mapping and pinning both exist to protect a body of existing
user work, and this fork does not have one to protect.

**Verified, and the config is the Studio one.** OCIO 2.5.2 in
`aswf/ci-vfxall:2027-clang21.1` publishes eight built-in configs, of which two
are ACES 2.0. Natron's default becomes:

    ocio://studio-config-v4.0.0_aces-v2.0_ocio-v2.5

not the CG config, and not the moving target `ocio://default` (which resolves
to the CG config today). Natron is a compositor, and Studio is the config aimed
at studio pipelines: 55 colorspaces against CG's 25, and — concretely — it
contains `Camera Rec.709`, the nearest equivalent to what Natron stores as
`"rec709"`. The CG config has no camera-referred Rec.709 at all, only display
curves. Pinning the exact name rather than `ocio://default` keeps out-of-the-box
colour behaviour from changing under us on an OCIO upgrade.

Read the identifier carefully: it embeds three independent versions. `v4.0.0` is
the config family's own colorspace-set version, `aces-v2.0` is the ACES spec
version, and `ocio-v2.5` is the minimum library version. They do not increment
together — there is no `v3.x` family.
