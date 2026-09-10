# Fork NatronGitHub/openfx to carry the clip and image metadata suite

2026-09-10. `libs/OpenFX` now points at `charlesangus/openfx-natron`, pinned
by exact commit, per
`PLAN/DECISIONS/2026-08-31-fork-and-fix-natrongithub-repos.md`.

## Why this can't live in `Engine/`

The metadata suite's host-side entry points — `getClipPropertySet`,
`getEffectPropertySet`, and the get/set property calls the suite adds to
`OfxPropertySuiteV2` — are implemented in
`libs/OpenFX/HostSupport/src/ofxhImageEffect.cpp`. The state those entry
points read and write is not incidental to that file; it belongs to the
classes defined there. The per-clip metadata cache lives on `ClipInstance`,
and the derivation of an output clip's metadata from its input clips'
metadata is a method on `Instance`, because `Instance` is the object that
owns the map of `ClipInstance*` (`_clips`) and knows the effect's clip
topology. Both are HostSupport classes, not `Engine/` classes.

There is no seam at which this could be built in `Engine/` instead.
`Engine/`'s node graph sits above HostSupport and calls into it through the
existing OFX suite plumbing; it has no access to `ClipInstance` internals
and no way to intercept a plugin's `OfxPropertySuiteV2` calls from outside
HostSupport. Adding the suite in `Engine/` would mean either duplicating
`Instance`/`ClipInstance` state there — a second copy of the same cache,
now with two sources of truth — or reaching down into HostSupport internals
from above, which is the kind of layering violation `Engine/` doesn't do
anywhere else. The suite has to be implemented where its state already
lives.

## What the fork carries, and where

`https://github.com/charlesangus/openfx-natron` holds `NatronGitHub/openfx`'s
full history plus the OpenFX clip and image metadata suite, added on top.
`libs/OpenFX` is pinned to it by exact commit
(`2303ff811bee3ffe085287602f684fe5fe5357e0`), not a branch and not a tag,
per the standing fork-and-fix policy.

The PR carrying the suite on the fork is based on a `natron-pin` branch at
`2303ff81` — the commit this submodule already pointed at — rather than on
the fork's `master`. Upstream `NatronGitHub/openfx`'s `master` has moved
three unrelated commits ahead of `2303ff81` since the fork was cut, and
rebasing the suite onto them would mean this submodule bump pulls in that
unrelated history too. Basing on `natron-pin` keeps the bump to exactly the
metadata suite and nothing else; the three commits can be picked up in a
later, separate bump if they're ever needed here.

## Upstreamable to ASWF `openfx`, with two known divergences

The suite was designed and reviewed in `charlesangus/openfx`, the
ASWF-lineage fork of `AcademySoftwareFoundation/openfx`, and transplanted
into `charlesangus/openfx-natron` unchanged in intent. It is meant to be
upstreamable to the ASWF project as-is. Two divergences exist between the
two copies, both forced by differences in the base lineage rather than by
choice:

- **`const char **` vs `char **` in `OfxPropertySuiteV2` string getters.**
  This lineage's `OfxPropertySuiteV1` already declares its string-getter
  outputs as `const char **`, so the V2 suite here follows suit. The ASWF
  copy's `OfxPropertySuiteV1` predates that constness and instead bridges
  the difference with an `APITypeConstless` typedef, which this lineage
  doesn't have and doesn't need. The two spellings are ABI-identical — same
  size, same calling convention — so this is a signature-level difference
  only, not a behavioral one.

- **A thread-safe metadata cache, where the ASWF original's is not.**
  Natron runs `eRenderSafetyFullySafe` renders with no serialising lock
  around plugin calls, and its render clones share clip objects outright:
  `Instance`'s copy constructor shallow-copies `_clips` with
  `_ownsData=false`, so a clone's `ClipInstance*` pointers are the same
  objects the original instance holds, not copies. Concurrent renders can
  therefore read and derive metadata on the same `ClipInstance` at once,
  which the ASWF host environment this suite was designed against doesn't
  do. The cache here adds a per-clip `std::mutex` and `std::atomic<int>`
  refcount, a per-`Instance` mutex serialising the get-metadata action
  itself, and a generation counter so that an invalidation racing an
  in-flight derivation isn't lost. None of this changes the suite's
  observable behavior for a single-threaded host — it only makes the cache
  safe under Natron's threading model.

## A note on the design

Metadata in this suite is per image, per frame — not per clip — matching
how OFX effects actually vary their output (region of definition, pixel
depth, and so on can change frame to frame). Values are restricted to int,
double, string, or arrays of those; there are no nested property sets and
no binary blobs, keeping the suite's property representation the same shape
as the rest of the OFX property system. There is deliberately no explicit
cache-invalidation property: the host invalidates on parameter changes and
clip changes, the same triggers that already invalidate other per-instance
state, rather than asking plugins to manage cache lifetime themselves.
