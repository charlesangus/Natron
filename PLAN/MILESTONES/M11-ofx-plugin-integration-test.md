# Milestone 11: OFX plugin integration test (pre-release)

> Stub — elaborate into phases/tasks before starting (PLAN-FORMAT.md §5).

M9 removes vendored third-party OFX plugins from the build and test path
because they were testing somebody else's binaries, in a container they cannot
load in, on the critical path of every PR. That was the right cut for the merge
gate — but "Natron can load and render through real OFX plugins" is still a
property that has to hold before anything ships. This milestone brings it back
where it belongs: an integration test run before a release, not a unit test run
on every push.

Roughly: obtain OFX plugin bundles that actually load on the EL9 baseline
(openfx-io for ReadOIIO/WriteOIIO, openfx-misc for the generators), stand up a
separate workflow that installs them, loads Natron against them, and renders a
known project to a golden image. It overlaps heavily with M5.P1.T2's headless
render regression test — the two should probably be designed together, with
this milestone supplying the plugins that test renders through.

Blocked on:

- **No EL9-compatible plugin build exists.** The only published asset is
  `openfx-io-build-ubuntu_22-testing.zip` (2023), which needs `GLIBCXX_3.4.30`
  and cannot load against Rocky 9's `libstdc++.so.6.0.29` — see M9's background
  section. openfx-misc publishes no testing build at all. Building either from
  source needs OIIO/OCIO/FFmpeg/SeExpr, which `aswf/ci-baseqt` does not ship,
  so this depends on settling a plugin-build toolchain (a fuller ASWF image, a
  side build, or our own published bundles).
- **The packaging decision** (`DECISIONS/2026-08-29-defer-packaging-decision.md`)
  — how Natron ships determines how plugins ship alongside it, and therefore
  what "installed plugins" means for this test to exercise.
- **M5's render regression work**, which defines the golden-image fixture and
  comparison this milestone would render through.

Acceptance sketch:

- A workflow separate from the PR gate — release-triggered or manually
  dispatched — installs OFX plugin bundles that load cleanly on the EL9
  baseline, with no `GLIBCXX`/`dlopen` failures in the log.
- Natron enumerates the expected plugin IDs at startup (at minimum
  ReadOIIO/WriteOIIO, plus a generator such as SeNoise) and the run fails loudly
  if any are missing — rather than the current behaviour of loading zero
  plugins and carrying on.
- A headless render through a real reader → effect → writer chain produces
  output matching a golden image.
- The plugin bundles are versioned and pinned, so a release is reproducible
  rather than dependent on whatever a moving `natron_testing` tag points at.
