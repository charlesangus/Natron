# M77 - Native Metadata Nodes

A small set of native nodes that edit the per-frame metadata from M74 - Native Metadata Core. Runs in `Engine/Nodes/Metadata/`.

## Phase 77.1: Nodes

- [ ] M77.P1.T1 — ModifyMetadata node
  - files: new `Engine/Nodes/Metadata/ModifyMetadata.h/.cpp`, `Engine/AppManager.cpp` (one registration line)
  - approach: a passthrough image node with a list of operations, each `set` / `remove` / `rename` on a key, with typed values. Values accept knob expressions, so they animate per frame. Overrides `getOutputMetadata`. The pixel path returns the input unchanged without copying where the framework allows (`isPointOp` / passthrough in `NativeImageEffect.h`).
  - verify: gtest: set, remove and rename keys on a Read → ModifyMetadata chain; unrelated keys survive; a keyframed value differs between frames.
  - size: M

- [ ] M77.P1.T2 — CopyMetadata and RemoveMetadata nodes
  - files: new `Engine/Nodes/Metadata/CopyMetadata.h/.cpp`, `Engine/Nodes/Metadata/RemoveMetadata.h/.cpp`, `Engine/AppManager.cpp`
  - approach: CopyMetadata takes pixels from input A and metadata from input B, with a key filter (all, glob list) and a merge mode (replace, merge-B-wins, merge-A-wins). RemoveMetadata drops keys by glob or removes all.
  - verify: gtest per mode and filter.
  - size: M

- [ ] M77.P1.T3 — ViewMetadata node and viewer display
  - files: new `Engine/Nodes/Metadata/ViewMetadata.h/.cpp`, `Gui/InfoViewerWidget.cpp` (or a new panel; check which fits), `Engine/AppManager.cpp`
  - approach: ViewMetadata is a passthrough whose panel lists every key and value at its position for the current frame. Reads `getOutputMetadata` for the displayed frame; refreshes when the frame or an upstream knob changes.
  - verify: Xvfb GUI check, with a screenshot shared before sign-off: Read an EXR with custom attributes, add the node, see the table; change the frame and see `ofx/frame` update.
  - size: M

- [ ] M77.P1.T4 — Node menu entries, help text and docs
  - files: `Engine/Nodes/Metadata/*.cpp` (descriptions only), `Engine/Nodes/README.md`
  - approach: group the four nodes under a `Metadata` menu, write the one-paragraph help text for each, and list the key prefix convention in the README.
  - verify: nodes appear under Metadata in the Tab menu with help text.
  - size: S

**Verification gate:** all four nodes covered by gtests; Xvfb GUI check with screenshot approved by the user; CI green.
