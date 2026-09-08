# 2026-09-05 — Adopt a bundle design for the .ntp successor format

**Decision.** The successor to the boost::serialization XML `.ntp` format is a
**stored-uncompressed zip container** (usdz/OpenRaster pattern) holding:

1. **A YAML graph member** — topology, knob values (sparse: defaults omitted),
   expressions, and light animation using RB-3's compact curve encoding
   (interpolation codes only on change, derivatives only for broken tangents).
2. **Binary blob members** for heavy per-node data — roto strokes, tracker
   keyframes, and any curve past a size threshold — as flat typed arrays (or
   FlatBuffers if schema evolution is wanted), 64-byte-aligned and stored
   uncompressed so they mmap zero-copy. Referenced by ID from the graph text.
3. **Explicit connections, serialized on the consumer node**, keyed by input
   *name* (not index), targets by fully-qualified script name
   (`Group1.Blur1`). Dangling references degrade to a disconnected input plus a
   warning, never a parse failure. Node order in the file is cosmetic (sort by
   script name for stable diffs); loading is order-independent, wiring happens
   in a second pass.

Saves keep the existing temp-file + rename atomic pattern. SQLite was the
runner-up — stronger on incremental save, crash recovery, and lazy load — but
forfeits hand-repair and text diffing, which weigh heavily for an open-source
compositor with pipeline users.

**Rationale / evidence** (investigation of 2026-09-05):

- *Current format costs.* `.ntp` is uncompressed XML via intrusive
  boost::serialization (`serialize()` on the Engine classes themselves, ~15
  files of hand-rolled `if (version >= X)` ladders). One XML element per
  keyframe (5 scalar fields each); one animated bezier control point carries
  **12 full curves** (x/y + tangents, duplicated for the feather point);
  RotoPaint strokes store one key per stroke sample across three parallel
  curves. `.nps` presets reuse `NodeSerialization` directly from
  `Gui/NodeSettingsPanel.cpp`. Boost's own archive version is a second compat
  axis outside our control (upstream NatronGitHub/Natron#824 broke project
  compat across a boost upgrade). Save path: `Project::saveProjectInternal()`
  (`Engine/Project.cpp`); load is eager and single-threaded via
  `NodeCollectionSerialization::restoreFromSerialization()`
  (`Engine/NodeGroupSerialization.cpp`).
- *Nuke's documented failure mode* is not TCL parsing but heavy data inlined as
  text plus whole-file rewrite on save/autosave: Foundry KB Q100199/Q100200
  advise disabling autosave for tracker-heavy scripts. Its positional
  push/stack wiring also makes rewire diffs non-local — Maya `.ma`'s explicit
  `connectAttr` diffs far better, hence explicit named connections here.
  (Natron's existing `Inputs_map` already uses input labels; this preserves
  those semantics.)
- *RB-3 prior art is directly reusable.* The abandoned Natron 3 branch
  (`origin/master`) has a standalone `Serialization/` library: yaml-cpp
  `encode`/`decode` decoupled from the Engine classes, the compact curve
  encoding above, and `SerializationCompat` — a boost-XML reader kept solely to
  import 2.x projects (the migration bridge this fork also needs). Its
  changelog claims ~10x smaller files, but it left strokes/tracker keys as
  inline text — it did not solve the heavy-data problem, hence the blob layer.
- *Container prior art.* usdz (openusd.org/release/spec_usdz.html) mandates
  stored-uncompressed, 64-byte-aligned zip members precisely for zero-copy
  mmap; OpenRaster/Krita prove the zip-with-manifest pattern for editing apps.
  A plain directory bundle diffs best but has no atomic save and reads as a
  folder on Linux/Windows; SQLite (sqlite.org/appfileformat.html) wins
  incremental save but is opaque to text tools.
- *Known limit.* This fixes file size, autosave stalls, and save cost. Opening
  a 10k-node project stays dominated by plugin instantiation until node
  creation is decoupled from deserialization (per-node
  `AppInstance::createNode` + O(K²) knob matching in `Node::loadKnob()`) —
  that decoupling is follow-on work, not part of the format itself.
