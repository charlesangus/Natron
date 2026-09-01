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

Open at decision time, and the first thing to check when implementing: that
ACES 2.0 specifically is among the built-in configs published by OCIO 2.5.2. If
it is not, the fallback is an external ACES config set, which reopens the
install-path work.
