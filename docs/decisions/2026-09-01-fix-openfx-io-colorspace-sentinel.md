# The ACES blocker is an openfx-io bug, fixed in our fork

Switching the default OCIO config to the ACES 2.0 built-in made a *fresh*
project fail to render with `Color space 'default' could not be found.` The
cause is not a missing role. It is a sentinel collision in openfx-io's
`canonicalizeColorSpace()` (`IOSupport/GenericOCIO.cpp:205-244`):

```cpp
int inputSpaceIndex = config->getIndexForColorSpace(csname.c_str());
...
} else if (inputSpaceIndex == defaultcs) {   // -1 == -1
    return OCIO::ROLE_DEFAULT;
```

`getIndexForColorSpace()` returns `-1` for "not found". When the queried name is
absent **and** the `default` role is absent, `-1 == -1` matches and the function
returns the string `"default"` — a name guaranteed not to exist, precisely
because its absence caused the match. That value is then baked in as a fresh
node's parameter default.

No ACES config of any vintage defines `default` or `reference`, and none defines
a colorspace named `sRGB` or `Rec709`. The old `blender` config never trips the
bug for an unrelated reason: it defines `reference: Linear` at index 0, the same
index as `scene_linear`, so the chain exits at its first branch. **It works by
luck, not by defining `default`.** Upstream `NatronGitHub/openfx-io` carries the
same code, untouched since a clang-format pass, so there is nothing to
cherry-pick.

The fix is ~41 lines in one file on our pinned `charlesangus/openfx-io` fork,
per the standing practice in
`2026-08-31-fork-and-fix-natrongithub-repos.md`: guard the `-1` sentinel; add a
validity fallback for parameter defaults preferring the `scene_linear` role over
the current "colorspace index 0" (which is `sRGB - Display` in every modern ACES
config — a display-referred fallback for a scene-referred pipeline); teach the
name lookup the ACES spellings; and fix a separate latent bug at
`GenericOCIO.cpp:958` that writes the literal `"default"` regardless of what it
just computed. Measured: the patch is a **no-op on the old config**, byte-identical
output, and it also repairs `OCIOColorSpace` and `OCIODisplay`, which are broken
on a fresh project today with nothing in CI exercising them.

**Rejected: substituting `scene_linear` wherever `default` appears.** It makes
the error go away and silently darkens every render by 2.5x — 46/255 instead of
118/255 for scene-linear 0.18 — because the linear value lands in an 8-bit sRGB
container with no encode. It would also relabel nuke-default's `raw` *data*
space as the scene-linear working space. `scene_linear` is right only as the
last-resort *fallback*, which is where the patch puts it.

Deriving a config in-process was the main alternative and remains the fallback
if third-party OFX plugins must work on the default config, since patching our
fork protects only our bundle. It is mechanically possible — `createEditableCopy()`
on a built-in works and serializes to 72 KB — but plugins can only be handed a
path or URI, so it forces a file on disk. An on-disk *wrapper* is impossible:
OCIO has no config inheritance, `search_path` locates LUTs rather than configs,
and `Config::CreateFromFile` rejects `.ociom`.
