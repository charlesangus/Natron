# M78 - Metadata In Expressions

Lets a knob expression read the metadata of any node's output: `thisNode.metadata('exr/owner')`, `node.metadata('ofx/frame')` and similar, with correct invalidation when the metadata changes.

## Phase 78.1: Python API

- [x] M78.P1.T1 — Add `Effect.metadata()` and `Effect.metadataKeys()`
  - files: `Engine/PyNode.h`, `Engine/PyNode.cpp`, `Engine/typesystem_engine.xml`
  - approach: `metadata(key, frame=currentFrame, view=0)` returns the typed Python value, or `None` when absent. `metadataKeys(frame, view)` lists keys. There is no unified accessor: skip pass-through nodes with `OfxMetadataBridge::skipPassThroughNodes`, then `dynamic_cast<NativeEffectBase*>` → `getOutputMetadata(time, view)`, else `OfxMetadataBridge::getOfxEffectOutputMetadata(effect, time)` (no view for OFX; ignore it), else empty — the same dispatch as the protected `NativeEffectBase::getInputEffectMetadata`, so factor a shared free helper rather than duplicating it. `ImageMetadata::Value` is a `std::variant<int, double, std::string, std::vector<int>, std::vector<double>>`; convert int→int, double→float, string→str, vectors→list, which needs a manual conversion in `typesystem_engine.xml` (the `Effect` object-type at ~line 1075). `getFrameRate` is only a signature model — it reads `NodeMetadata`, a different system.
  - freshness (2026-10-19): `Effect::getFrameRate` at `PyNode.h:411`; `NativeEffectBase::getOutputMetadata` at `NativeEffectBase.h:334`; `getInputEffectMetadata` at `NativeEffectBase.cpp:272`. No PyNode gtest exists yet; `Tests/Native/NativeWriteMetadata_Test.cpp:66-90` generates attribute-carrying EXRs with OIIO, and `Tests/NativeMetadataTestEffect.h` injects metadata.
  - verify: gtest/Python script: Read an EXR with attributes; `app.Read1.metadata('exr/owner')` returns the value; absent key returns `None`; a Python integer-typed key stays an `int`.
  - size: M

## Phase 78.2: Dependencies and invalidation

- [ ] M78.P2.T1 — Register metadata reads as expression dependencies
  - files: `Engine/Knob.cpp` (`parseListenersFromExpression`, `executeExpression`), `Engine/Knob.h`
  - approach: the current scan rewrites `getValue`-style calls to `addAsDependencyOf`. Add the same treatment for `metadata(...)`/`metadataKeys(...)`: record the referenced node as a metadata dependency of the knob, then clear the knob's `_exprRes` when that node's metadata is invalidated. Keep it consistent with the read-set scan M71 - RFV Render Context plans, which is another text scan on `Knob.cpp`; do not build a second mechanism. If M71 has not landed, record the dependency in a small list that M71 can fold in.
  - freshness (2026-10-19): M71 has not landed. A metadata read is not a knob listener, so the `addAsDependencyOf` path can't carry it: a new node-level edge is needed from the referenced node B to the expression's knob on A. `EffectInstance::onNodeHashChanged` (`EffectInstance.cpp:5904`) already clears `_exprRes` on A's own hash change, and B's hash only reaches A when B is upstream. So the edge must (a) clear A's `_exprRes` when B's hash changes — hook on B's `onNodeHashChanged`, which covers OFX nodes too, unlike the native-only `invalidateOutputMetadata` — and (b) feed B into A's content hash, so the cache doesn't reuse A's renders; see `Node::getHashDependents`/`getKnobsContentDigest` (`Node.cpp:867`) and `getAllExpressionDependenciesRecursive` (`Knob.cpp:5254`). The text scan is `KnobHelperPrivate::parseListenersFromExpression` (`Knob.cpp:2610`).
  - verify: gtest: a knob expression reads `Read1.metadata('ofx/frame')`; changing the Read's file or a ModifyMetadata value upstream changes the expression result without a project reload.
  - size: L

- [ ] M78.P2.T2 — Time-varying metadata and the expression cache
  - files: `Engine/Knob.cpp`, `Engine/KnobImpl.h`
  - approach: `_exprRes` is keyed by time only, so an expression using metadata at the current frame is already correct, but one asking for another frame's metadata must key on that frame. Decide, record the choice in this milestone's `## Decisions`, and either bypass the cache for metadata-reading expressions or key it. Metadata that depends on an RFV is out of scope, as stated in the RFV design. Note `_exprRes` (`Knob.h:2458`, lookups at `KnobImpl.h:486/515/574/603`) ignores the view too, even though `executeExpression` receives one; key on `(time, view)` if that falls out naturally.
  - verify: gtest: `metadata('ofx/frame', frame + 1)` returns the next frame's value after a frame change.
  - size: M

- [ ] M78.P2.T3 — Documentation and an example project
  - files: `Engine/Nodes/README.md`, a test project fixture under `Tests/`
  - approach: document the API and key prefixes as a subsection of the existing "Per-frame metadata" section (README line ~107); add a fixture that drives a Text-type knob (a label) from a metadata key and renders in ctest. Generate the EXR at test time with OIIO rather than checking in a new asset.
  - verify: fixture loads and the evaluated label matches the file metadata.
  - size: M

## Decisions

- 2026-10-19 — `_exprRes` stays keyed by time only (M78.P2.T2): a read of another frame's metadata is still a deterministic function of the evaluation time, and any source hash change clears the whole cache through the P2.T1 edge. View stays out of the key: `Knob::getValueAtTime` hardcodes `ViewIdx(0)`, so keying on view needs a wider fix first.
- 2026-10-19 — Metadata dependencies are a node-level edge (M78.P2.T1), refreshed when the source's hash is recomputed (pull-driven). Changes that don't move the source hash (project format, frame rate) don't refresh readers. `Engine/Knob*` is touched, so the gate runs the GUI bench.

**Verification gate:** API and invalidation gtests pass; headless project load evaluates a metadata expression correctly; CI green.
