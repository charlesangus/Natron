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

## Phase 72.4: RFVSwitch

Placed before Phase 72.3 so the UAT covers it. Design: `DECISIONS/2026-10-08-rfvswitch-design.md`.

- [ ] M72.P4.T1 — Pattern matcher for RFVSwitch
  - files: `Engine/Nodes/RFV/RFVPatternMatch.h`, `Engine/Nodes/RFV/RFVPatternMatch.cpp` (new), `Engine/CMakeLists.txt`, `Tests/RFVPatternMatch_Test.cpp`
  - approach: `int firstMatch(const std::string& value, const std::vector<std::string>& lines)` returns the index of the first matching non-empty line or -1. A line starting `re:` is an ECMAScript regex matched against the whole value; any other line is a glob (`*`, `?`, `[abc]`, `[!abc]`) matched against the whole value. An invalid regex never matches and is reported through an out-parameter so the node can show it. Empty lines never match but still occupy their input index.
  - verify: gtest table: globs, regexes, first-wins ordering, empty lines, invalid regex.
  - size: M

- [ ] M72.P4.T2 — RFVSwitch node
  - files: `Engine/Nodes/RFV/RFVSwitch.h`, `Engine/Nodes/RFV/RFVSwitch.cpp` (new), `Engine/AppManager.cpp`, `Engine/CMakeLists.txt`
  - approach: native polymorphic-kind node with a fixed set of optional inputs (16, hidden until a line exists, following how existing multi-input native nodes declare inputs; check `Engine/Nodes/README.md`). Knobs: "Variable" (string), "Patterns" (multi-line string; line i selects input i), "If no match" (Choice: Error / First input). `isIdentity` reads `rfv` for Variable under the request-pass context, converts it with `str()` (arrays and unset convert to a no-match), runs `firstMatch`, and returns the matching input. `getExtraRFVReads()` returns the Variable's value so the node's context key includes it. Unset or non-matching with "Error" sets a render error naming the variable and its value.
  - verify: gtest: three Checkerboards of different sizes feed the switch; with the project variable `shot=sh010` and patterns `sh01*` / `re:sh0[2-9]0` the first input is passed; changing the variable to `sh030` passes the second.
  - size: M

- [ ] M72.P4.T3 — RFVSwitch under two Set nodes
  - files: `Tests/RFVSwitch_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: the user's two-branch graph with an RFVSwitch below a shared upstream: Writer A under Set(`var='a'`), Writer B under Set(`var='b'`) must each pick a different input, with `renderBothWays` bit-identical. Plus a diamond where one branch hits and the other falls to "First input". Assert via `CountingTestEffect` that only the selected inputs render.
  - verify: the new tests pass under both schedulers.
  - size: M

## Phase 72.5: RFVRead

Placed before Phase 72.3 so the UAT covers it. Design: `DECISIONS/2026-10-08-rfvread-design.md`.

- [ ] M72.P5.T1 — Resolve the context a node would see, without rendering
  - files: `Engine/RFV.h`, `Engine/RFV.cpp`, `Engine/ParallelRenderArgs.cpp` (reuse only)
  - approach: `RFV::resolveContextAt(NodePtr target, double time)` finds a downstream path from `target` to the active viewer's displayed input chain (the first path found when several exist; if the target is on no viewer's path, the project base values), then folds the Set-node overlays along that path, nearest to the target winning, evaluated at `time`. Use the same overlay function the request pass uses (extract it if `M71.P3.T1` left it inline) so the two cannot drift. Returns the full context plus the name of the path's output node.
  - verify: gtest: user's two-Set example with the viewer on each branch gives `a` then `b`; a node on no viewer path gives the project values; a nested override resolves nearest-first.
  - size: M

- [ ] M72.P5.T2 — RFVRead node
  - files: `Engine/Nodes/RFV/RFVRead.h`, `Engine/Nodes/RFV/RFVRead.cpp` (new), `Engine/AppManager.cpp`, `Engine/CMakeLists.txt`
  - approach: polymorphic pass-through like `ReverseFlowVariableSet` (identity, no render). It reads nothing, so it does not widen any upstream context key. One read-only multi-line string knob, "Variables", lists every variable in the resolved context as `name = value` (arrays as `[1, 2, 3]`, strings quoted) with a header line `from <output node>, frame <t>`. Recompute via `RFV::resolveContextAt` on timeline change, viewer change, connection change, and any change to a Set node or the project RFV group. Variables whose value is unset on this path are not listed.
  - verify: gtest: build the user's example, call the node's refresh with the viewer on each branch, and compare the knob text to the expected lines.
  - size: M

- [ ] M72.P5.T3 — RFVRead refresh in the GUI
  - files: `Gui/NodeGraph*.cpp` or the viewer-change signal source (grep for the active-viewer-input changed signal), `Engine/Nodes/RFV/RFVRead.cpp`
  - approach: hook the active-viewer and displayed-input change into RFVRead's refresh so the panel updates without a render. Keep the knob read-only and selectable for copy.
  - verify: Xvfb GUI run: move the viewer between the two branches of the example and screenshot the panel showing `var = 'a'` then `var = 'b'`.
  - size: M

## Phase 72.3: UAT

- [ ] M72.P3.T1 — Package an AppImage and capture screenshots
  - files: `build/appimages/` (output), `Tests/gui/` (an Xvfb script, if needed)
  - approach: package a release AppImage with `package.sh`. Under Xvfb, load a fixture project with the user's two-Set example, capture screenshots of the Set node panel, the project Variables section, and the viewer on each branch. Send the shots to the user.
  - verify: viewer shows different results for the two branches; the user has the screenshots.
  - size: M

- [ ] M72.P3.T2 — User check of the AppImage
  - files: none
  - approach: the user opens the AppImage and builds the example graph, checks that an RFVRead lists the right variables for the viewed branch, checks that an RFVSwitch picks inputs by glob and regex, checks that animated variables work, that a project variable is overridden by a Set, and that save and reload keeps everything. Record their findings as tasks or decisions.
  - verify: the user signs off.
  - size: S

**Verification gate:** full ctest passes; the user has signed off the AppImage check (M72.P3.T2); decisions from this feature are published to `docs/decisions/`.
