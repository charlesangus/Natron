# M78 - Metadata In Expressions

Lets a knob expression read the metadata of any node's output: `thisNode.metadata('exr/owner')`, `node.metadata('ofx/frame')` and similar, with correct invalidation when the metadata changes.

## Phase 78.1: Python API

- [ ] M78.P1.T1 — Add `Effect.metadata()` and `Effect.metadataKeys()`
  - files: `Engine/PyNode.h`, `Engine/PyNode.cpp`, `Engine/typesystem_engine.xml`
  - approach: `metadata(key, frame=currentFrame, view=0)` returns the typed Python value, or `None` when absent. `metadataKeys(frame, view)` lists keys. Both resolve through the node's output metadata for both native nodes (M74 - Native Metadata Core) and OFX nodes (clip metadata). Shiboken binding entries follow the neighbouring `getFrameRate` methods.
  - verify: gtest/Python script: Read an EXR with attributes; `app.Read1.metadata('exr/owner')` returns the value; absent key returns `None`; a Python integer-typed key stays an `int`.
  - size: M

## Phase 78.2: Dependencies and invalidation

- [ ] M78.P2.T1 — Register metadata reads as expression dependencies
  - files: `Engine/Knob.cpp` (`parseListenersFromExpression`, `executeExpression`), `Engine/Knob.h`
  - approach: the current scan rewrites `getValue`-style calls to `addAsDependencyOf`. Add the same treatment for `metadata(...)`/`metadataKeys(...)`: record the referenced node as a metadata dependency of the knob, then clear the knob's `_exprRes` when that node's metadata is invalidated. Keep it consistent with the read-set scan M71 - RFV Render Context plans, which is another text scan on `Knob.cpp`; do not build a second mechanism. If M71 has not landed, record the dependency in a small list that M71 can fold in.
  - verify: gtest: a knob expression reads `Read1.metadata('ofx/frame')`; changing the Read's file or a ModifyMetadata value upstream changes the expression result without a project reload.
  - size: L

- [ ] M78.P2.T2 — Time-varying metadata and the expression cache
  - files: `Engine/Knob.cpp`, `Engine/KnobImpl.h`
  - approach: `_exprRes` is keyed by time only, so an expression using metadata at the current frame is already correct, but one asking for another frame's metadata must key on that frame. Decide, record the choice in this milestone's `## Decisions`, and either bypass the cache for metadata-reading expressions or key it. Metadata that depends on an RFV is out of scope, as stated in the RFV design.
  - verify: gtest: `metadata('ofx/frame', frame + 1)` returns the next frame's value after a frame change.
  - size: M

- [ ] M78.P2.T3 — Documentation and an example project
  - files: `Engine/Nodes/README.md`, a test project fixture under `Tests/`
  - approach: document the API and key prefixes; add a fixture that drives a Text-type knob (a label) from a metadata key and renders in ctest.
  - verify: fixture loads and the evaluated label matches the file metadata.
  - size: S

**Verification gate:** API and invalidation gtests pass; headless project load evaluates a metadata expression correctly; CI green.
