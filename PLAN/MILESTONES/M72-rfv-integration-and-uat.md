# M72 - RFV Integration And UAT

Reverse-Flow Variables, part 3 of 3 (design in `DECISIONS/2026-10-08-reverse-flow-variables-design.md`). Depends on M71 - RFV Render Context. Covers the places a variable is read outside a render, the documented v1 limits, and the user acceptance check.

## Phase 72.1: Reads outside a render

- [ ] M72.P1.T1 — Refresh expression results when a project variable changes
  - files: `Engine/Knob.cpp` (`parseListenersFromExpression`), `Engine/RFV.cpp`, `Engine/Project.cpp` (`onKnobValueChanged`)
  - approach: when an expression's read-set names a project variable, register the project knob as a dependency (listener) of the reading knob, so parameter panels refresh and `knobsAge` bumps. Variables set by a Set node are not tracked as dependencies, because the value depends on the path. Check that a project RFV change reaches nodes that read it without a restart.
  - verify: gtest: change a project variable and the reading knob's displayed value and the node hash both update.
  - size: M

- [ ] M72.P1.T2 — Metadata that depends on RFVs: define and test the v1 behaviour
  - files: `Tests/RFVMetadata_Test.cpp` (new), `Tests/CMakeLists.txt`, `Engine/Nodes/README.md`
  - approach: build a node whose format/frame-range knob uses `rfv`; render under a Set node and confirm that nothing crashes and metadata is computed from the base values. Document the limit in `Engine/Nodes/README.md` next to the RFV section added by M72.P2.T1.
  - verify: the test passes under both schedulers.
  - size: M

## Phase 72.2: Docs

- [ ] M72.P2.T1 — Document RFVs for users and node authors
  - files: `Engine/Nodes/README.md`, `docs/decisions/` (publish copy happens at the gate per PLAN-FORMAT.md §3a)
  - approach: a short section: what an RFV is, the Set node, the project group, the `rfv` object (attribute and call forms, `None` when unset), resolution time (frozen at the Set node), per-path semantics, and the v1 limits. Follow the repo's comment policy for code comments.
  - verify: docs build check (if any) is clean; the example from the task text runs verbatim in a test.
  - size: S

## Phase 72.3: UAT

- [ ] M72.P3.T1 — Package an AppImage and capture screenshots
  - files: `build/appimages/` (output), `Tests/gui/` (an Xvfb script, if needed)
  - approach: package a release AppImage with `package.sh`. Under Xvfb, load a fixture project with the user's two-Set example, capture screenshots of the Set node panel, the project Variables section, and the viewer on each branch. Send the shots to the user.
  - verify: viewer shows different results for the two branches; the user has the screenshots.
  - size: M

- [ ] M72.P3.T2 — User check of the AppImage
  - files: none
  - approach: the user opens the AppImage and builds the example graph, checks that animated variables work, that a project variable is overridden by a Set, and that save and reload keeps everything. Record their findings as tasks or decisions.
  - verify: the user signs off.
  - size: S

**Verification gate:** full ctest passes; the user has signed off the AppImage check (M72.P3.T2); decisions from this feature are published to `docs/decisions/`.
