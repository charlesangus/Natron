# Expression results, expression dependencies and links

State recorded on branch `milestone/m242-cache-invalidation-audit`. "Fixed by M202" means PR #131
(`origin/milestone/m202-clean-hash-cache-stores`) closes the entry. "M71" means PR #122
(`origin/milestone/m71-rfv-render-context`). "M72" means
`origin/milestone/m72-rfv-integration-and-uat`. Paths are relative to `Engine/` unless stated.

## Caches

| Cache | Owner | Lifetime |
| --- | --- | --- |
| `_exprRes` (expression results by time) | `Knob<T>` Knob.h:2859 | Until `clearExpressionsResults` (Knob.h:2753-2760) |
| `_exprCtxRes` (results by time, view and context key) | `Knob<T>` beside `_exprRes` | As `_exprRes` |
| `_exprResGenerations` | Knob.h:2863 | Moved by every clear; guards stores (KnobImpl.h:538-547) |
| Metadata reader registry | `Node::addMetadataReader` Node.cpp:1750, `getMetadataReaders` :1790 | Until `removeMetadataReader` (Knob.cpp:3773) |
| Metadata hash sources | `Node::appendMetadataSources` Node.cpp:1181, cached by generation | Until the hashed-sources generation moves (Knob.cpp:3538-3575) |
| Link groups | `KnobLinkGroup` (KnobLinkGroup.h:60-65) | Group lifetime; holds the shared value, animation and expression, members hold copies |

Links hold no result cache: master/slave knobs read the master live, and a link group writes its
state into every member (`adoptLinkGroupState` Knob.cpp:5968-6016).

## Key

- `_exprRes` is keyed by time only. All callers pass `ViewIdx(0)` (KnobImpl.h:762, :881, :957), so
  the missing view is consistent. `_exprCtxRes` adds view and context key.
- `getValueFromExpression` (KnobImpl.h:482-552) reads the stored result, evaluates on a miss and
  stores only if the dimension's generation did not move during evaluation (:538-547). The `_pod`
  variant (:590-660) shares the maps.

### What moves a node's hash when a knob read through an expression or link changes

- `Knob::getContentHash` (Knob.cpp:6712-6723) folds `getDimensionContentHashRecursive` per dimension.
- `getDimensionContentHashRecursive` (Knob.cpp:6755-6830) hashes the knob's own state
  (`getOwnDimensionContentHash` Knob.cpp:7146: stored value, curve, expression string, has-return
  flag, validity), then the masters (:6762-6766), then every expression dependency recursively
  (:6803-6813, the append at :6812), then the metadata sources' revisions (:6814-6830, plus the
  session salt when non-zero). Volatile tokens are folded at :6729-6731.
- **A whole-value dependency contributes nothing.** The recursion returns 0 for `dimension < 0`
  (Knob.cpp:6758). Whole-value dependencies come from `parseTokenFrom` (Knob.cpp:2503), which
  writes `-1` for an empty dimension argument (:2573-2575) into `.addAsDependencyOf` (:2591), then
  `Param::_addAsDependencyOf` (PyParameter.cpp:964-985) and `addListener` (Knob.cpp:5143, :5191).
  That is E1.
- `Node::getKnobsContentDigest` (Node.cpp:876-947) re-hashes every knob that reads outside itself
  (expression, master, or unmemoized) on every call, so a changed dependency is seen without an
  explicit dirty mark. It also reports `hasExpression`.
- Masters and listeners: `invalidateContentHash` (Knob.cpp:6915) walks listeners
  ([keys.md](keys.md)).

### Renames

- `NodeName.cpp:429-474` rebinds the Python variable (`setNodeVariableToPython`), then rewrites the
  expressions of every listener with `replaceNodeNameInExpression` (Knob.cpp:3425: a plain string
  replace, then `setExpression`) and of every metadata reader with
  `replaceNodeNameInExpressionCode` (Knob.cpp:3458, identifier- and path-aware).
  `onScriptNameChanged` runs at NodeName.cpp:477-483.
- The content hash folds the fully qualified name only when the node has an expression
  (Node.cpp:970-972, keys.md step 4), so a rename re-keys exactly the nodes whose expressions
  could resolve names. Tests: `Tests/NodeContentHash_Test.cpp:246`, `:262`.
- Rewritten expressions change the expression string, so the readers' hashes move.

### Metadata-reading expressions

- `addMetadataDependency` (Knob.cpp:3538-3575) registers the knob on the source node with
  `addMetadataReader`, unless the source is the knob's own node and the knob is render-hashed
  (:3572-3573). It moves the hashed-sources generation.
- `appendMetadataSources` (Node.cpp:1181) feeds the sources into the hash inputs (`getHashInputs`,
  Node.cpp:1149 for the viewer, :1169; `knobMetadataReadsFoldIntoHash` :1173) and into the cycle
  walk (:1256). `expressionReadClosesMetadataCycle` (Knob.cpp:3643) refuses a read that closes a cycle.
- `refreshMetadataReaders` (Node.cpp:1846-1898) handles a metadata change under the same hash. It
  clears the node's own render-hashed knobs that read it, then clears the readers' results and
  invalidates their content hash, then queues `evaluateValueChange`. Callers:
  `onMetadataMayHaveChanged` (Node.cpp:1805), `onMetadataChangedUnderSameHash` (:1809, outputs at
  :1829), :1836, and NativeEffectBase.cpp:485.

## Invalidated by (when `_exprRes` is cleared)

- `EffectInstance::onNodeHashChanged` (EffectInstance.cpp:6174-6200) clears every knob's results,
  but only when `mayHaveKnobExpressions()` (Node.cpp:1742-1747: digest not valid, or it has an
  expression). It also calls `onMetadataMayHaveChanged` (:6184).
- `refreshListenersAfterValueChange` (Knob.cpp:4866-4918) clears a listener's results when the
  dimension matches, or either side is `-1` (:4888, clear :4909-4911). Callers: Knob.cpp:2081,
  :8296, KnobImpl.h:2841, :2878, :2976, :3007.
- `clearExpression` (Knob.cpp:3783), `Curve::onCurveChanged` (Curve.cpp:2034),
  `refreshMetadataReaders` (above), and `Node::deactivate` (Node.cpp:4347-4358).
- `clearExpressionsResultsIfNeeded` (Knob.cpp:1796, called KnobImpl.h:3123) is dead: the flag it
  tests is only ever set false (Knob.cpp:678, :1803).

### Does a link alone keep stale results?

No. Link groups and master/slave links store no results. A link-group member's hash follows its
own copied value (`copyStateToLinkGroup` Knob.cpp:6060-6086, `adoptLinkGroupState` :5968-6016,
sync via `beginSync`/`endSync` KnobLinkGroup.cpp:288-326), and a slave's follows its master
(Knob.cpp:6762-6766). An expression on a linked knob is cleared by the listener path above.

### frame and time expressions

An expression that reads `frame` stores one result per time, which is right. Whether the node's
images are keyed per time is a separate question: see I1 in [image-caches.md](image-caches.md)
(a frame expression does not make the node frame-varying). Dynamic names are K4 and K6 in
[keys.md](keys.md) (keys.md:248).

## Not invalidated by

- Results in a knob whose node has no expression: none are stored, so nothing to clear.
- A whole-value dependency changing: `_exprRes` is cleared (the `-1` filter at Knob.cpp:4888), but
  the reader's content hash does not move (E1), so image and action caches keyed by it hit.

## Covered by tests

- Seeded stress (`Tests/CacheStress.h`): `CacheStressTest.GradeBlurTransformWithExpressionsAndLinks` and `GradeBlurTransformInsideAGroup` set and clear per-dimension expressions and link and unlink knobs; `CacheStressSelfTest.TheReferenceEqualsAFreshAppLoadOfTheSavedProject` checks the reference against a saved and reloaded project.
- `Tests/Native/MetadataExpressionDependency_Test.cpp:130-731` (about 40 cases): reader hash follows
  the source (:183), same-hash metadata changes (:277-344), results not kept across an
  invalidation (:375, :395), cycles (:435-575), renames (:677-731).
- `Tests/Native/NativeReadReload_Test.cpp`, `Tests/Metadata_Test.cpp:536`.
- `Tests/NodeContentHash_Test.cpp:246`, `:262` (rename re-keys only expression nodes).
- `Tests/KnobLinkGroup_Test.cpp:166-749` (29 cases; :373, :446), `Tests/LinkedNode_Test.cpp:110-708`
  (19 cases), `Tests/PyLinks_Test.cpp`.
- On M72, `Tests/RFVExpression_Test.cpp:277` (`ProjectVariableEditRefreshesTheReaderAndItsNodeHash`)
  pins E1.

## M202 and M71

- M202 does not change expression hashing. Its store gate catches an edit landing during a render,
  not a hash that never moves, so it does not close E1.
- M71 adds context-keyed results (`_exprCtxRes`) and the context key in the render hash.
- M72 (commit 68d7a7125 on `origin/milestone/m72-rfv-integration-and-uat`) makes a whole-value
  dependency hash every dimension of its target, closing E1 for values.

## Suspected holes

- **E1 Whole-value expression dependencies add nothing to the hash.**
  `getDimensionContentHashRecursive` returns 0 for `dimension < 0` (Knob.cpp:6755, :6758), and
  `.addAsDependencyOf(-1, ...)` is what an expression reading a whole parameter registers
  (Knob.cpp:2573-2575, :2591). Editing the target leaves the reader's node hash unchanged, so its
  cached images and actions are served. **Fixed by M72** (68d7a7125, test
  `Tests/RFVExpression_Test.cpp:277` on that branch). **Not fixed by M202.** Residual on M72:
  `appendHashedMetadataSources` (Knob.cpp:3596-3600) still returns for `dimension < 0`, so metadata
  sources reached through a whole-value dependency are not folded.
  Stress case: `CacheStressTest.DISABLED_WholeValueExpression_M72`; `eOpWholeValueExpression` is out of `defaultOps()`.
- **E2 Rename rewrites listeners by plain string replace.** `replaceNodeNameInExpression`
  (Knob.cpp:3425) replaces the old name as a substring, unlike the identifier-aware path for
  metadata readers (:3458). A name that is a substring of another identifier can be rewritten
  wrongly; the new expression string then moves the hash, so no cache serves stale content, but
  the value is wrong. `NodeName.cpp` does not call `recheckInvalidExpressions` (callers are
  Node.cpp:4581, Knob.cpp:860, ProjectPrivate.cpp:263, NodeMain.cpp:716, NodeInputs.cpp:852,
  AppInstance.cpp:2777). Correctness notes, not cache holes. The early return for an empty
  expression that left `beginChanges()` unmatched, blocking every later evaluation of the node, is
  **fixed in M242** (the check now comes first); test:
  `ExpressionFrameVaryingTest.RenameWithoutAnExpressionLeavesNoBracketOpen`. The plain string
  replace stays on the follow-up list.
- **E3 Clamped and unclamped results share entries.** The stored result is clamped or not by the
  caller's `clamp` (KnobImpl.h:538-539); the pod path passes `false` (KnobImpl.h:957). A value
  outside min and max read both ways returns whichever was stored first. Minor. **Not fixed by
  M202.**
