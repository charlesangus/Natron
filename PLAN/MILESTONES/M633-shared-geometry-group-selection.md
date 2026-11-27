# M633 - Shared Geometry Group Selection

Extract the Group / Group Type logic into one shared component that every native geometry node uses, so no node reinvents it or differs from the others in how it presents or applies the rule (user, 2026-11-25, during M600 - Native Geometry UAT Round 2). Today a helper does part of the parsing (`Engine/Geo/GeoOpUtils.cpp:1330`, declared `GeoOpUtils.h:146`), called from `GeoDelete.cpp:76-80`, `GeoDisplace.cpp:274-276`, `GeoToUSD.cpp:755-757` and `GeoTransform.cpp:293-295`. But each node declares its own group knobs, and GeoUVProject only got a Group Type knob in M600. When in doubt, match Houdini's group-parameter behaviour.

## Scope

**In:**
- One component that owns:
  - the Group and Group Type knobs, their labels, tooltips and Guess-from-Group resolution;
  - the rule that a blank group means every element of the class;
  - mapping a point or primitive selection onto points, vertices or primitives;
  - leaving unselected elements untouched.
- Every Geo* node that takes a Group moved onto it, with one shared test.

**Out:**
- The viewer group picker (M608 - Geometry Group Picker), which consumes this component later.

**Edge cases:**
- **Group names a class that differs from Group Type:** convert it the same way in every node (a point group on a primitive op selects the primitives whose points are all in it, matching Houdini's default), and document the rule in the tooltip.
- **Saved projects:** no backward compatibility is needed; knob script names may stay the same where they already agree.

## Phase 633.1: Shared component

- [ ] M633.P1.T1 — Audit every Geo* node's group knobs and selection rules
  - files: read-only: `Engine/Geo/*.cpp`, `Engine/Nodes/Geo*` (every node with a group knob, e.g. GeoTransform, GeoUVProject, GeoDisplace, GeoWrangle, GeoDelete, GeoToUSD); `.local/docs` Houdini group docs; table to `build/m633/group-audit.md`
  - approach: per node, list knob script names, labels, defaults, accepted group classes, Guess behaviour and how a selection maps to the element class it modifies. Note where they disagree and what Houdini does.
  - verify: the table covers every Geo* node with a group knob, with file:line
  - size: M
- [ ] M633.P1.T2 — Write the shared group component and its test
  - files: new `Engine/Geo/GeoGroupSelection.{h,cpp}` (or extend `GeoOpUtils`), `Engine/CMakeLists.txt`; new `Tests/Native/GeoGroupSelection_Test.cpp`
  - approach: a class that declares the Group/Group Type knobs on a node (labels from script names), resolves Guess from the group's class, and returns a selection for the requested element class (points, vertices, primitives) using T1's agreed rules. Bundled gtest is old: no `GTEST_SKIP`, no `ASSERT_TRUE(ptr)`.
  - verify: the test covers blank group, each class-to-class mapping, Guess and unselected elements, all green
  - size: L

## Phase 633.2: Move the nodes onto it

- [ ] M633.P2.T1 — Move every Geo* node with a group knob onto the shared component
  - files: each node from T1's table (`Engine/Nodes/Geo*` / `Engine/Geo/*`), and their existing node tests
  - approach: replace each node's group knobs and selection code with the component, keeping its script names. Remove the old per-node helpers once unused.
  - verify: every node's existing tests pass; a parametrised test runs the same group cases through each moved node
  - size: L

**Verification gate:** full build and ctest green; Xvfb screenshot of two nodes' Group knobs side by side showing identical presentation, sent to the user.
