# Maintainer notes (pending distribution)

These notes are a temporary holding pen. They still need to be distributed into
`CONTRIBUTING.md` and into comments at the relevant headers, after which this
file should be deleted.

## Ownership and lifetime

- Parent-to-child links are `shared_ptr`; child-to-parent back-references are
  `weak_ptr`. This avoids reference cycles that would otherwise leak the whole
  object graph. Follow this convention when adding relationships between
  classes (verified against current use of `std::enable_shared_from_this` in
  `Engine/Knob.h`, `Engine/Project.h`, `Engine/RotoContext.h`, and others).
- Classes that need to hand out a `shared_ptr` to themselves derive from
  `std::enable_shared_from_this` (for example `KnobI` in `Engine/Knob.h`).
  Never construct a second `shared_ptr` from a raw `this`.
- PIMPL (`FooPrivate`) is used deliberately: it keeps public headers small and
  stable, so changing a private member does not force a recompile of every
  includer, and it keeps heavy or platform-specific includes out of the public
  interface. When adding state to a class, put it in the `…Private` object,
  not the public header.
- Include a `Fwd` header (`Engine/EngineFwd.h`, `Gui/GuiFwd.h`) instead of a
  class's full header whenever you only need to name the type. This is a
  major reason the project compiles at all given its size.
- `KnobI` is deliberately **not** a `QObject`: knobs must be lightweight,
  copyable, and are created in large numbers, which rules out `QObject`.
  To still emit change signals, each knob owns a `KnobSignalSlotHandler`
  (confirmed present in `Engine/Knob.h`) — a companion `QObject` that emits
  the knob's signals on its behalf. Follow this "signal-handler companion"
  pattern for any other non-`QObject` core class that needs to notify
  observers, rather than converting the class to a `QObject`.

## Architecture: why Node and EffectInstance are separate

`Node` is the durable graph vertex — stable identity, connections, undo
history. `EffectInstance` is the (possibly replaceable) rendering behavior
behind it. Keeping them apart is what lets Natron reset or reload a plug-in,
swap a Read node's decoder when the file type changes, or run several render
clones of the same effect concurrently — all without disturbing the graph
topology the user built.

## Headless operation drives the architecture

The single most important force shaping the architecture is that the same
Engine must run without a GUI (`NatronRenderer`, for command-line and
render-farm use). Any change in `Engine` must build and behave correctly
without `Gui`. If you find yourself wanting to call into `Gui` from `Engine`,
that's a signal to add a method to one of the abstract `…I` interfaces
instead (implemented on the `Gui` side) rather than including a `Gui` header.

## Rendering

- Per-render context (captured node hashes, the abort flag, timing/statistics)
  is captured up front and carried in thread-local storage
  (`ParallelRenderArgs`, installed by `ParallelRenderArgsSetter`), not read
  from live node state. The two classic ways to break rendering: (a) reading
  live node state during a render instead of the captured hash/args, and
  (b) forgetting to install or restore `ParallelRenderArgsSetter` on a new
  code path. When adding a render entry point, mirror an existing one
  exactly.
- Any in-flight render can be aborted. Check the abort flag periodically in
  long-running render loops, or the UI feels stuck.
- Cache entries are keyed by a 64-bit hash (`Hash64`) computed from
  everything that affects the result (parameters, input hashes, time, view,
  scale, region). If an input changes, the hash changes and the old entry is
  simply not found — this is why cache invalidation stays correct with no
  explicit dirty-tracking.

## Editing operations and views

- Implement editing operations as undo commands rather than mutating state
  directly, or undo silently breaks.
- Plumb `ViewIdx` through render code exactly as the surrounding code does;
  dropping it silently breaks stereo projects, and the type system will not
  catch the mistake.

## OpenFX host

- Triage heuristic: a bug that reproduces with *all* plug-ins points at the
  host glue (`Engine/Ofx*` or `HostSupport`); a bug that reproduces with only
  *one* plug-in is probably in that plug-in's own repository, not here.
- The host-to-plug-in contract is effectively a public API: third-party and
  commercial plug-ins depend on it. Prefer additive, capability-flagged
  changes over breaking changes.
- A stale or corrupt OFX plug-in cache is a likely cause when a plug-in
  appears missing or stale after an update; clearing it forces a full
  rescan.

## Serialization and compatibility

- Any change to a `…Serialization` struct can break existing users' project
  files. Always bump the class version (`BOOST_CLASS_VERSION`) and add a
  version-guarded branch that can still read the old layout; never silently
  change field meaning or order. This is the single easiest place to cause
  data-loss regressions.
- The `Py*` API (user scripts, PyPlugs, tutorials) is similarly
  compatibility-critical: keep it stable and minimal, and treat breaking it
  as seriously as breaking the serialization format.
- When fixing a bug that's unit-testable (color, image, curve, hashing,
  geometry), add a regression test. Rendering and GUI behaviour are harder to
  unit-test; describe manual test steps in the PR instead.

## Build and toolchain

- `DEBUG` builds enable floating-point exception trapping at startup
  (`Global/FloatingPointExceptions.h`, confirmed present), so a stray NaN or
  divide-by-zero aborts at the source instead of silently propagating through
  the image pipeline.
- `QT_NO_CAST_FROM_ASCII` is defined project-wide (confirmed in
  `CMakeLists.txt`): you cannot implicitly build a `QString` from a
  `const char*`. Wrap literals in `QString::fromUtf8(...)`.
- Install third-party Python packages into Natron's bundled interpreter
  (`natron-python -m pip install <pkg>`) rather than the system Python, so
  the package lands on the interpreter Natron actually runs (verified:
  `PythonBin/CMakeLists.txt` still builds this binary as `natron-python`,
  and `Documentation/source/_environment.rst` documents the same usage).

## Qt 6 / QRegExp porting rule (historical — call sites already migrated)

This project has already completed its Qt 6 migration and removed `QRegExp`
entirely (verified: no `QRegExp` usage remains in `Engine` or `Gui`; all
sites now use `QRegularExpression`, including
`QRegularExpression::wildcardToRegularExpression()` in
`Engine/FileSystemModel.cpp`, `Gui/NodeCreationDialog.cpp`,
`Gui/PreferencesPanel.cpp`, `Gui/NodeGraph45.cpp` and
`Gui/RenderStatsDialog.cpp`). The mapping rule that guided that migration is
worth keeping, since it no longer has old call sites to read it back from:

`QRegExp` had two matching modes: `indexIn()` found a pattern *anywhere* in
the string, while `exactMatch()` required the *whole* string to match.
`QRegularExpression::match()` is find-anywhere and has **no** `exactMatch`
equivalent. So old `indexIn` sites map to `match()` directly, but old
`exactMatch` sites must anchor the pattern (`\A…\z`, or
`QRegularExpression::anchoredPattern()`). Separately, `QRegExp::WildcardUnix`
maps to `QRegularExpression::wildcardToRegularExpression(pattern)`, which was
flagged as the one conversion needing real thought (case sensitivity and
anchoring behave differently from the old wildcard mode).

## Conventions

- Numbered source files (`Gui20.cpp`, `NodeGraph45.cpp`, `ViewerTab30.cpp`,
  …) are one class split across several files purely to keep translation
  units a manageable size. The numbers carry no semantic meaning beyond
  grouping related methods.
