# M570 - Scene Query Expression API

Lets a knob expression read a scene node's output through objects: `USDRead1.scene()` gives a `Scene`, `scene.prim("/World/Cam")` a `ScenePrim`, and the prim's getters read it at a frame: `prim.worldTransform()`, `prim.bound()`, `prim.attribute("radius").get(frame)`. A 2D Transform can then track a 3D prim, or a matte can be sized from a bound. M78 - Metadata In Expressions is the model. `Effect::metadata()` (`Engine/PyNode.{h,cpp}`, bound in `Engine/typesystem_engine.xml` with `modify-function` inject-code that returns a `PyObject*`) reads at `KnobHelper::getExpressionEvaluationTime()`. The dependency scan in `Engine/Knob.cpp` (`extractMetadataReads`, ~:3466) rewrites the calls into `addMetadataAsDependencyOf`, which records a node-level edge (`metadataDependencies`, ~:461). That edge clears the reader's expression results when the source's hash changes, and its cycle check refuses a read whose source folds the reader's knob. A scene is pulled with `EffectInstance::evaluateScene(EvaluateSceneArgs)` (`Engine/EffectInstanceRenderScene.cpp`) into a `ScenePayload`. It is read through `SceneQueryPool::borrow(payload, loadSet, …)` leases (`Engine/Nodes/Scene/SceneQueryPool.h`) and SceneOps readers such as `readAttribute(stage, path, name, frame, framesPerSecond, …)`. Python-facing files include no pxr headers, as `Engine/Nodes/Scene/SceneCaches.h` already arranges for code built under `Py_LIMITED_API`. The query code therefore lives in `Engine/Nodes/Scene/`, behind a pxr-free header.

The API (exact shape; `frame=None` means the expression's evaluation time, else the node's current time; frames map to time codes with the scene's frames per second, as `SceneOps::readAttribute` does):

```python
scene = USDRead1.scene(view=0)            # Effect.scene(); ValueError if the node outputs no scene; no payload loaded
scene = scene.load("/World/Set", "/World/Cam", descendants=True)   # a new Scene that also loads those payloads
scene.loadedPaths()                       # the paths this Scene's load() calls named, with their descendants flag
scene.upAxis(); scene.metersPerUnit(); scene.timeCodesPerSecond(); scene.defaultPrim()
scene.renderSettings()                    # ScenePrim of the active render settings prim
scene.renderCamera(frame=None)            # ScenePrim its camera relationship targets
prim = scene.prim("/World/Cam")           # no read yet; also scene.prim(path) for any string
prim.path(); prim.name(); prim.parent()   # path arithmetic, no read
prim.exists(frame=None); prim.closestAncestor(frame=None)
prim.typeName(); prim.kind(); prim.isActive(); prim.purpose()
prim.isVisible(frame=None); prim.invisibleAncestors(frame=None)        # list of paths
prim.children(frame=None)                 # list of ScenePrim
prim.worldTransform(frame=None, invert=False)                          # 16 floats, Gf row-major
prim.localTransform(frame=None, invert=False)
prim.relativeTransform(other, frame=None, invert=False)                # other: ScenePrim or path
prim.translate(frame=None, space="world", relativeTo=None)             # (x, y, z)
prim.rotate(frame=None, space="world", relativeTo=None, order="XYZ")   # degrees
prim.scale(frame=None, space="world", relativeTo=None)
prim.bound(frame=None, space="world", relativeTo=None, purposes=("default",))  # ((min), (max))
prim.boundCenter(...); prim.boundSize(...)                             # same arguments
prim.material(purpose="")                 # bound material path, "" when none
prim.metadata(key)                        # value or None
prim.fieldOfView(frame=None, axis="horizontal"); prim.apertureAspectRatio(frame=None)
prim.frustum(frame=None)                  # (left, right, bottom, top) at distance 1
prim.projectPoint(point, frame=None)      # camera prim: (x, y, depth), x and y in 0..1 of the screen window
attr = prim.attribute("radius")           # SceneAttribute, no read yet
attr.exists(frame=None); attr.get(frame=None, default=None); attr.typeName()
attr.isAuthored(); attr.timeSamples(); attr.source(frame=None)         # "fallback", "default", "timeSamples", "valueClips", "spline", "none"
prim.primvar("displayColor", inherit=True)                            # SceneAttribute, indices applied
prim.relationship("material:binding").targets()                       # list of paths
```

## Scope

**In:**
- `Scene.load(*paths, descendants=True)`, the only way an expression loads payloads. It returns a new `Scene` whose load set includes the payload at each path and those on the way to it, plus every payload below it when `descendants` is True. Calls chain and accumulate: `scene.load("/a").load("/b")` loads both. The `Scene` it was called on is unchanged, and `ScenePrim`s taken from a `Scene` read with that `Scene`'s load set. The same load set is used in the viewer, in renders, in writes and in scripts, so a value never depends on who evaluates it.
- `Effect.scene()`, and the `Scene`, `ScenePrim`, `SceneAttribute` and `SceneRelationship` Python types above, as copyable Shiboken value types holding only a node weak pointer, a view, path strings and the load paths, so that holding one costs nothing and reads nothing.
- Value conversion: bool, int, float, str, token, asset path (resolved path string), Gf vectors, quaternions and matrices as tuples, and arrays as lists. An unsupported type raises `TypeError` naming it.
- Dependency registration, invalidation and cycle refusal for `scene(` reads on the M78 node-level edge.
- A per-frame query result cache, the threading and cancellation rules in Edge cases, and `App.waitForSceneQueries(timeoutMs)` for scripts, modelled on `App.waitForReloads`.
- Docs: `Engine/Nodes/Scene/docs/SceneExpressions.md`, plus inject-documentation on every bound method.
- Reference query audit. These are Gaffer's query nodes, mapped onto methods rather than nodes, since the user chose an expression API:
  - TransformQuery: `location` → `prim()`; `space` Local/World/Relative → `localTransform`/`worldTransform`/`relativeTransform` and the `space` argument; `relativeLocation` → `other`/`relativeTo`; `invert` → `invert`; outputs `matrix`, `translate`, `rotate`, `scale` → the matching methods. All In.
  - BoundQuery: `location`, `space`, `relativeLocation`; outputs `bound`, `center`, `size` → `bound`, `boundCenter`, `boundSize`. All In. `purposes` is USD's own addition, matching Solaris's `usd_getbbox` purpose argument.
  - AttributeQuery: `location`, `attribute`, `inherit`, `default`; outputs `exists`, `value` → `attribute(name)`, `exists()`, `get(default=)`. USD attributes don't inherit, so `inherit` is on `primvar()`, the one kind that does. All In.
  - ExistenceQuery: `exists`, `closestAncestor`. In.
  - VisibilityQuery: `visible`, `invisibleAncestors`. In, from computed visibility.
  - CameraQuery: `cameraMode` Render Camera/Location → `scene.renderCamera()` or `scene.prim()`; `queries` → `attribute(name)`; the built-in `apertureAspectRatio`, `fieldOfView` and `frustum` → methods; the per-query `source` output → `attribute.source()`. All In.
  - PrimitiveVariableQuery: `queries` with defaults → `primvar(name).get(default=)`. In.
  - OptionQuery: render options are attributes of the render settings prim → `scene.renderSettings().attribute(name)`. In.
  - ShaderQuery: a shader parameter is an attribute of the shader prim → `attribute("inputs:<name>")` on it. In, no extra method.
  - SetQuery, FilterQuery: Out; they need pattern matching (see Out).
  - PointInstancerQuery: Out (see Out).
  - UDIMQuery, PrimitiveQuery: Out; they cover topology and texture tiles, which are spreadsheet data (M337 - Geometry Spreadsheet), not single values.

**Out:**
- Pattern and wildcard queries, set or collection membership, and scene traversal beyond `children()`: no milestone. They would match across unloaded payloads.
- Writing to the scene from Python: no milestone. Edits are nodes.
- Exposing OpenUSD's own Python (`Usd.Stage`) to expressions: no milestone.
- Query nodes in the graph (a TransformQuery node): no milestone. The user chose an expression API.
- Per-instance PointInstancer queries: no milestone.
- Classifying `scene(` reads for the time-independent cache: M264 - Expression Result Cache, which must treat them as time-dependent, as it does metadata.
- Native evaluation of scene calls: M265 - Native Expression Evaluator. Scene calls stay on the Python path.

**Edge cases:**
- Payload loading: naming a prim loads nothing. `USDRead1.scene()` composes with no payload loaded (`SceneLoadSet::none()`), and only `load()` adds payloads (`SceneLoadSet::withPaths` for the paths and the prims on the way, plus an `eRuleAll` entry at each path for `descendants=True`). The viewer's load set is never read, and renders do not load everything for an expression. A prim inside a payload the expression did not load reads as missing (`exists()` False, defaults elsewhere), its ancestors up to the payload prim read normally, and a `bound` counts loaded prims only. The node doc says so with an example.
- `load()` on a path inside another unloaded payload: the payloads on the way are loaded, as `withPaths` does, so `load("/World/Set/Tree")` works without loading `/World/Set` first. A path that names no prim after loading is not an error: its queries read as missing.
- `load()` reaches payloads that are deeper than the scene's current prims only through paths. Patterns are not accepted (see Out).
- Evaluation context: queries evaluate with one dedicated context id, so the nodes' memoized layers for viewers are not evicted.
- Off the main thread (render threads): queries run synchronously. A `PythonGILUnlocker` releases the GIL around `evaluateScene`, the borrow and the read, so upstream expressions can run and other Python is not stalled.
- On the main thread inside an expression: a cache hit returns at once. A miss starts the query on a worker, coalesced per key, and returns the query's default: an identity matrix, an empty bound, False, `None` or `default`. A thread-local pending flag stops `getValueFromExpression` from storing that result in `_exprRes`. When the worker finishes, the readers' expression results are cleared and their knobs refresh, as a metadata change does. A script calling `knob.get()` calls `app.waitForSceneQueries()` first.
- Called directly from a script (outside any expression): synchronous, as a script expects a value.
- Cancellation: inside a render, `evaluateScene` checks the render's abort flag between steps. A worker query carries its own `AbortableRenderInfo`, aborted when the project closes, when the queried or the reading node is deleted, or when a newer request replaces it. A borrow under way finishes its composition step before the abort is seen.
- Missing prim or attribute: getters return the defaults above, as Gaffer's queries do. `exists()` tells them apart.
- A node with no scene output: `scene()` raises `ValueError`. A failed evaluation raises `RuntimeError` with the failing node's message, which marks the expression invalid as any error does.
- Cycle: an expression on A reading the scene of a node whose hash folds A's knob is refused, with the M78 cycle message.
- Disabled or identity nodes: `evaluateScene` hands back the input's payload, so they read through.
- Instance proxies: readable, since a `UsdPrim` at the path is the proxy.
- Multi-view: `scene(view)` evaluates at that view. Results are cached per view.
- `relativeTo` on a missing prim: the defaults, as for a missing prim.
- `rotate` on a sheared matrix: decomposed with `GfMatrix4d::Factor`, with the shear dropped, as TransformQuery's rotate output does.

**Assumptions:**
- Each query needs one lease: `relativeTransform` and `relativeTo` name prims of the same scene.
- `KnobHelper::getExpressionEvaluationTime()` is set on every path that evaluates an expression, render threads included.
- Clearing `_exprRes` from a worker completion, posted to the main thread, reaches every reader through the existing metadata-dependency path.

## Phase 570.1: Query core

- [ ] M570.P1.T1 — Prim, existence and transform queries
  - files: `Engine/Nodes/Scene/SceneExpressionQuery.{h,cpp}` (new; pxr-free header), `Engine/CMakeLists.txt`, `Tests/Scene/SceneExpressionQuery_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: a pxr-free `SceneExpressionQuery::Request {node, view, loadPaths, path, kind, args, frame}` and `Result` (a small variant: bool, double, string, string list, 16 doubles, two triples). `run(request, abortInfo, &result, &error)` evaluates the node's scene with `evaluateScene` under the dedicated context id and the load set built from `loadPaths` (none when empty), borrows from `SceneQueryPool`, and answers `exists`, `closestAncestor`, `typeName`, `kind`, `isActive`, `purpose`, `children`, `local`/`world`/`relative` transforms with `invert`, and `translate`/`rotate`/`scale` in each space. Frames convert to time codes as `SceneOps::readAttribute` does.
  - verify: gtest on scenes built with `Tests/Scene/SceneTestAuthoring.cpp`: USDXformPrim → USDCamera with an animated translate gives the right world matrix at two frames; a relative transform between two prims; invert; a decomposed rotate in each order; `closestAncestor` of a missing child; a prim inside an unloaded payload reads as missing without `load()`, and after `scene.load(path, descendants=False)` it is found with only the payloads on its path loaded (checked through the borrowed stage's load rules); a chained `load()` accumulates and leaves the original `Scene` unchanged.
  - size: L

- [ ] M570.P1.T2 — Attribute, primvar, relationship and metadata queries
  - files: `Engine/Nodes/Scene/SceneExpressionQuery.{h,cpp}`, `Tests/Scene/SceneExpressionQuery_Test.cpp`
  - approach: add kinds for `attribute.get`/`exists`/`typeName`/`isAuthored`/`timeSamples`/`source`, primvar reads through `UsdGeomPrimvarsAPI::FindPrimvarWithInheritance` with indices applied (`ComputeFlattened`), relationship targets, and prim metadata. Values go into a pxr-free tagged value (`SceneExpressionValue`: scalar kinds, tuples and lists) so the Python side converts without pxr. `source` maps `SceneOps::SceneValueSource`.
  - verify: gtest: a schema `radius` with no opinion reads its fallback and `source()=="fallback"`; an authored animated attribute lists its samples; a primvar inherited from an ancestor with `inherit=True` and not with `False`; an indexed primvar comes back flattened; a `point3f[]` comes back as a list of triples; a matrix attribute as 16 floats; a missing attribute returns the default.
  - size: M

- [ ] M570.P1.T3 — Bound, visibility, material and camera queries
  - files: `Engine/Nodes/Scene/SceneExpressionQuery.{h,cpp}`, `Tests/Scene/SceneExpressionQuery_Test.cpp`
  - approach: `bound`/`boundCenter`/`boundSize` with `UsdGeomBBoxCache` (given purposes, `useExtentsHint` on) in local, world or relative space, over the prims the `Scene`'s load set includes. Also: `isVisible` and `invisibleAncestors` from `ComputeVisibility` and the ancestors' authored `invisible`, `material` via `UsdShadeMaterialBindingAPI::ComputeBoundMaterial(purpose)`, the render settings prim and render camera, and the camera computations: `fieldOfView` and `apertureAspectRatio` (from `GfCamera`), `frustum` at distance 1, and `projectPoint` through the camera's frustum.
  - verify: gtest: a sphere's world bound after a translate; a bound over an unloaded payload counts only the loaded prims, and the same bound from `scene.load(path)` counts the payload's geometry; an invisible ancestor is listed; a bound material path; a 50 mm camera on a 36 mm aperture gives about 39.6°; `projectPoint` of a point on the camera axis gives (0.5, 0.5, distance).
  - size: M

- [ ] M570.P1.T4 — Query result cache
  - files: `Engine/Nodes/Scene/SceneExpressionQuery.{h,cpp}`, `Engine/Nodes/Scene/SceneCaches.h` (purge entry point), `Tests/Scene/SceneExpressionQuery_Test.cpp`
  - approach: a bounded, MT-safe LRU keyed on (payload stack hash, load set hash, view, path, kind and arguments, time code), filled by `run` and dropped on a cache purge and project close beside the pool's idle stages. Concurrent requests for one key wait on the first instead of computing twice.
  - verify: gtest: the same query twice composes once (count borrows through a test hook); a different frame misses; an upstream edit changes the payload hash and misses; 8 threads querying one key compute it once and all get the value.
  - size: M

## Phase 570.2: Python API

- [ ] M570.P2.T1 — Python types and `Effect.scene()`
  - files: `Engine/PyScene.{h,cpp}` (new), `Engine/PyNode.{h,cpp}`, `Engine/typesystem_engine.xml`, `Engine/CMakeLists.txt` (generated wrapper list), `Tests/Native/ScenePythonQuery_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: copyable value types `Scene`, `ScenePrim`, `SceneAttribute`, `SceneRelationship` holding a node weak pointer, view, strings and the `Scene`'s load paths (a sorted list of path and descendants flag, turned into a `SceneLoadSet` per request). `Scene.load` returns a copy with the paths added. Each getter builds a `Request`, takes `frame` from the argument, else `KnobHelper::getExpressionEvaluationTime`, else the node's current time, runs it under a `PythonGILUnlocker`, and converts the `SceneExpressionValue` to Python. Bind each method with `modify-function` returning `PyObject*` and set `ValueError`/`RuntimeError`/`TypeError` as `Effect.metadata` does, with inject-documentation for every method. Synchronous for now; P2.T3 adds the main-thread path.
  - verify: gtest on the embedded interpreter (harness of `Tests/Native/MetadataExpressionDependency_Test.cpp`): every method in the API block returns the expected Python type and value on a small scene; `scene()` on a Blur raises `ValueError`; a failing upstream raises `RuntimeError` with its message; a `ScenePrim` outlives its node and then raises `RuntimeError` saying the node is gone.
  - size: L

- [ ] M570.P2.T2 — Register scene reads as expression dependencies
  - files: `Engine/Knob.cpp`, `Engine/PyNode.{h,cpp}`, `Tests/Native/ScenePythonQuery_Test.cpp`
  - approach: extend `extractMetadataReads` to `scene` calls so they become `addMetadataAsDependencyOf(thisParam)` on the node they are made on, reusing the node-level edge, its hash-driven clear and its cycle check. Rename nothing.
  - verify: gtest: a Double knob with `USDCamera1.scene().prim("/camera1").translate()[0]` updates after the camera's translate knob changes, without a reload; an expression on USDEditTransform1 reading its own downstream USDMerge's scene is refused with the cycle message; deleting the read node marks the expression invalid.
  - size: M

- [ ] M570.P2.T3 — Main-thread queries that never block
  - files: `Engine/PyScene.cpp`, `Engine/Nodes/Scene/SceneExpressionQuery.{h,cpp}`, `Engine/KnobImpl.h` (pending flag check), `Engine/Knob.{h,cpp}`, `Engine/PyAppInstance.{h,cpp}`, `Engine/typesystem_engine.xml`, `Tests/gui/scene_expression_query.py` (new)
  - approach: on the main thread inside an expression, a cache miss posts the request to a worker (coalesced per key, its own `AbortableRenderInfo` aborted per Edge cases), sets a thread-local pending flag and returns the default. `getValueFromExpression` and its variants skip storing a result whose evaluation set the flag. On completion, the main thread clears the readers' expression results through the dependency edge and refreshes their knobs. `App.waitForSceneQueries(timeoutMs=60000)` processes events until none are pending.
  - verify: Xvfb script at 1920x1080: a Transform's translate expression tracks a USDCamera's world translate; dragging the camera's translate knob updates the Transform knob within a timeout; a main-thread heartbeat stays within the M333 stall budget while a root bound over the Island flat file with `scene.load("/")` runs; `waitForSceneQueries()` then `get()` returns the real value; closing the project with a query in flight neither crashes nor hangs.
  - size: L

## Phase 570.3: Docs

- [ ] M570.P3.T1 — Scene expressions doc and example fixture
  - files: `Engine/Nodes/Scene/docs/SceneExpressions.md` (new), `Engine/Nodes/README.md` (link beside the metadata section), `Tests/Native/ScenePythonQuery_Test.cpp`
  - approach: document every method with its arguments, defaults, return shapes, `load()` and the rule that nothing else loads payloads, main-thread behaviour and `waitForSceneQueries`, in the style of the existing Scene docs, with no reference-app names. Add a test that loads a small project in which a Text knob shows a prim's `typeName` and a Transform follows a camera, and checks both values headless.
  - verify: the fixture test passes; the doc lists every method bound in `typesystem_engine.xml` (a grep of method names in the test).
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `SceneExpressionQuery_Test` and `ScenePythonQuery_Test` pass; `Tests/gui/scene_expression_query.py` passes under Xvfb at 1920x1080; the GUI bench (`tools/bench/gui/run.sh --xvfb`, `compare.py` against `tools/bench/gui/baselines/xvfb.jsonl`) shows no regression and is recorded in `## Decisions`, since `Engine/Knob*` is touched.

## Decisions

- 2026-11-24 — **Object-style API (user):** expressions get a scene node, then its prims, whose attributes are Python attributes or getters ("get a scene node and then its attributes are Python attributes or getters"), not flat functions such as `scene.transform(node, path)`. The planner chose the exact shape above.
- 2026-11-24 — **Payloads load only through an explicit load action (user):** naming a prim loads nothing; queries see only what is loaded unless the expression calls the load action, in the viewer and in renders alike. The planner designed it as `Scene.load(*paths, descendants=True)` returning a new `Scene`.
- 2026-11-24 — **Main thread not ready (user, accepting the recommendation):** a miss returns an uncached default and the knob refreshes when the worker finishes; scripts call `App.waitForSceneQueries()` first.
