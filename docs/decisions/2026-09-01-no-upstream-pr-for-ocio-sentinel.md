# The OCIO sentinel guard stays on our fork; no upstream PR

`M3.P1.T12` was written to land the `-1` sentinel guard in
`canonicalizeColorSpace()` as its own commit and offer it to
`NatronGitHub/openfx-io`, on the grounds that it is obviously correct and
config-agnostic. Asked at the task boundary, the user chose **not to open the
upstream PR**.

The commit is still deliberately self-contained
(`charlesangus/openfx-io@60c0275`, `+7/-0`, no dependency on the ACES-specific
changes that follow it), so the option stays open at zero cost — cherry-pick it
onto a clean branch whenever that changes. Nothing in this fork depends on
upstream taking it: `fetch-assets.sh` pins our fork's SHA, per
`2026-08-31-fork-and-fix-natrongithub-repos.md`.

This narrows, but does not overturn, `2026-08-29-upstream-agnostic-fixes.md` —
platform-agnostic fixes remain worth upstreaming as a bridge to
lockewerks/cedricp; this particular one is simply not being pushed now.
