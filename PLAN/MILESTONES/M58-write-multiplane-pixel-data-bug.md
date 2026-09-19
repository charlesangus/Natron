# Milestone 58: Write's "All Layers" output copies one layer's pixel data into every layer

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

Discovered while diagnosing M57.P1.T2, then bisected out of that milestone because it predates
M39 (confirmed pre-existing on `3e14c2a1e`, byte-for-byte identical symptom before and after the
plane→layer rename — see M57's `## Decisions`). With a Write node's "All Layers" checkbox
(`processAllLayers` OFX param) checked, the rendered multi-part/multi-layer EXR gets the correct
per-subimage layer *names* (e.g. `diffuse.R/G/B`, `specular.R/G/B`, `R/G/B/A`) but every
subimage contains **identical pixel data** — specifically, whichever layer got rendered/copied
last (`diffuse` in the repro) ends up duplicated into all of them, silently discarding the
others' actual content. This is a correctness bug in Natron's own multi-layer image-copy path
(`Engine/EffectInstance.cpp`'s `Implementation::renderHandler` and/or `Engine/OfxClipInstance.cpp`'s
`getOutputImageInternal`, per M57.P1.T2's abandoned investigation — not yet root-caused) or
possibly in the `charlesangus/openfx-io` fork's `WriteOIIO`/`GenericWriter` render loop; scope
unconfirmed.

Blocked on: nothing technical — this is a plannable, self-contained bug fix. Stub only because
it surfaced mid-milestone and needs its own elaboration pass (root-cause investigation first,
same as M57.P1.T2 attempted) rather than being folded into M57's scope. No user go-ahead needed
beyond normal backlog prioritization (see the board's backlog-reorg note for where this slots
in — it was not part of that reorg and needs an explicit priority decision).

Reusable artifacts from the abandoned M57.P1.T2 investigation (all cleaned up, not preserved on
disk — reproduce fresh):
- Repro recipe: build a single-part EXR with 2+ layers using **visually-distinct solid colors**
  per layer (e.g. `oiiotool --chappend` of separately-colored constant images) — a names-only
  check via `oiiotool -info` is NOT sufficient to catch this bug, since the layer *names* come
  out correct; only a pixel-value comparison (`oiiotool --info -v -a` plus an actual pixel dump,
  or a Python OIIO read) reveals the wrong data.
- The bug reproduces via the plain headless `NatronRenderer` Python API
  (`app.createReader`/`createWriter`/`connectInput`/`render`) — no GUI needed.
- `Engine/EffectInstance.cpp`'s `Implementation::renderHandler` (the `outputLayers` map
  iteration) and `Engine/OfxClipInstance.cpp`'s `getOutputImageInternal` (which looks up an
  output image by layer name in that same map) are the two most promising places to instrument
  first — a mismatch between the key used to populate `outputLayers` and the key used to look
  an image back up for each OFX-side layer request would produce exactly this symptom (every
  lookup resolving to the same entry).
- A second investigation pass ruled out plane *declaration*/negotiation as the culprit:
  `WriteOIIOPlugin::getClipComponents` (`build/openfx-io-fork/OIIO/WriteOIIO.cpp:568-590`) takes
  the `eGetPlaneNeededRetCodeReturnedAllPlanes` branch and calls
  `_inputClip->getPlanesPresent(&components)` correctly — channel names/subimage count always
  matched the input across several layouts tried. Since names come out right but *data* doesn't,
  the bug is downstream of negotiation, in the per-plane pixel fetch: look at
  `GenericWriterPlugin::render`'s call to `_inputClip->fetchImagePlane(time, view, plane.c_str(),
  ...)` (`build/openfx-io-fork/IOSupport/GenericWriter.cpp` ~line 423) and whether the requested
  plane *name* actually threads through to the correct source buffer in the host-side
  `OfxClipInstance::getInputImageInternal` (`Engine/OfxClipInstance.cpp`) — i.e. whether every
  plane's fetch resolves to the same (wrong) buffer regardless of the name it asked for.

Acceptance sketch:
- A Write node with "All Layers" checked, given 2+ distinctly-valued upstream layers, produces
  an output where each subimage's pixel data matches its own layer's actual content (not another
  layer's).
- Existing 216-test ctest suite and the smoke test stay green.
