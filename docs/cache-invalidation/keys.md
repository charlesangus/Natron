# Cache keys: the node hash and the render hash

Every engine cache keys its entries on a node's hash, directly or through the frame args. This page
describes how that hash is built, when a render captures it, and how an edit moves it. Citations are
`file:line` on branch `milestone/m242-cache-invalidation-audit`, with paths relative to `Engine/`
unless they start with `Tests/`. "M202" is PR #131 (`milestone/m202-clean-hash-cache-stores`,
unmerged). "M71" is PR #122 (`milestone/m71-rfv-render-context`, unmerged).

## Caches

This page covers the key, not a store. The stores keyed on it are mapped in
[actions-and-metadata.md](actions-and-metadata.md), [image-caches.md](image-caches.md) and the Phase
242.2 docs. The node-level state this page owns:

| State | Where | What it is |
| --- | --- | --- |
| Node content hash | `Node::getContentHash` Node.cpp:804, `getStoredContentHash` Node.cpp:817 | Memoized per node and recomputed lazily when the node is dirty. |
| Dirty mark | `markHashDirtyInternal` Node.cpp:1407, `isHashDirty` Node.cpp:1586, `getHashMarkCount` Node.cpp:1574 | Per-node generation plus the global epoch `hashEpoch()` Node.cpp:1276. |
| Dirty queue | `flushHashDirtyMarks` Node.cpp:1488 | Marks are queued and propagated to dependents in batches, when a hash is read. |
| Project digest | `Project::getContentDigest` Project.cpp:3460 | Memoized on the project knobs, formats, layers, OCIO source and concatenation generations. |
| Upstream context read-set | `Node::getUpstreamContextReadSet` NodeInputs.cpp:2101 | Memoized per `getRenderContextReadsGeneration()` RenderContext.cpp:231. |
| Frame args `nodeHash` | `ParallelRenderArgsSetter::buildArgsMap` ParallelRenderArgs.cpp:1944 | The hash a render captures per node, held in TLS for the frame. |

## Key

### Content hash

`Node::computeContentHash` (Node.cpp:950) folds in, in this order:

1. Plug-in ID, major and minor version, and `Plugin::getBinaryContentStamp()` (Node.cpp:955-959).
2. `getKnobsContentDigest(&hasExpression)` (Node.cpp:965, defined at 876). This covers only
   knobs for which `KnobHelper::isRenderHashed()` is true (Knob.cpp:4228): content-hashed, not
   hash-exempt, and persistent or evaluate-on-change. Knobs are taken in name order. A knob's own
   hash (`getDimensionContentHashRecursive` Knob.cpp:6755) follows its master and expression
   dependencies (`getContentHashDependencies` Knob.cpp:7117).
3. `effect->appendNodeContentHash(h)` (Node.cpp:966). The default is empty (EffectInstance.h:403).
   Overrides: USDTransform.cpp:262, USDRead.cpp:273, AddLayers.cpp:375, DeepAddLayers.cpp:353.
   Each pairs with a `markHashDirty()` where its private state changes (USDTransform.cpp:257,
   USDRead.cpp:232, AddLayers.cpp:370, DeepAddLayers.cpp:349).
4. `hasExpression`, and the node's fully qualified name if it is set (Node.cpp:970-972). A rename
   therefore re-keys only nodes with expressions (`Tests/NodeContentHash_Test.cpp:246`, `:262`).
5. Inputs. A viewer folds both active inputs by slot, with 0 for an empty one (Node.cpp:976-980).
   Other nodes fold only connected slots plus their count (Node.cpp:984-987). Empty slots are left
   out on purpose (comment at Node.cpp:1158-1161). Inputs come from `getHashInputs` (Node.cpp:1133).
6. The roto context hash and the paint-stroke item hash (Node.cpp:992-999).
7. For each enclosing group: its disabled, lifetime-enabled and lifetime knobs (Node.cpp:1007-1030).
8. The project frame range, only if `hasExpression || effect->dependsOnProjectFrameRange()`
   (Node.cpp:1035-1041).
9. The project digest (Node.cpp:1044).

`getHashInputs` (Node.cpp:1133) reads `getInput(i)`, which resolves through
`applyNodeRedirectionsUpstream` (NodeInputs.cpp:78). That function looks through a Group to its
output's input (:87), a precomp to `PrecompNode::getOutputNode()` (:93; PrecompNode.cpp:147 returns
the read node when Pre-Render is on), and a GroupInput to the group's real input (:103). It skips
the RotoPaint context node for paint strokes and appends the metadata sources from
`appendMetadataSources` (Node.cpp:1181).

The hasher is XXH3 (`ContentHasher.cpp:67`) with no seed. Doubles are hashed by bit pattern
(`appendDouble` ContentHasher.cpp:118).

### Render hash

`EffectInstance::getHash()` (EffectInstance.cpp:466) is the node's live hash.
`EffectInstance::getRenderHash()` (EffectInstance.cpp:535) returns the hash captured in the frame
args at the back of the thread's TLS (`request->nodeHash` if set, else `nodeHash`). Acting under
another node's context, it mixes that context's key into the context-free args hash (:548-553).
With no frame args in TLS it falls back to the live `getHash()` (:539-541).

### When the render hash is captured

`ParallelRenderArgsSetter`'s constructor (ParallelRenderArgs.cpp:1908) runs `buildArgsMap` (:1936)
before it installs the TLS. `buildArgsMap` (:1944) reads `node->getHashValue()` for each node in
turn (:2004, roto paint nodes :2018, multi-instance children :2034). Then `snapshotMetadata` (:1811)
compares each node's metadata hash with its captured `nodeHash`. Moved nodes get their `nodeHash`
re-read, up to `kMaxMetadataSnapshotRetries` = 3 times (:1767, :2052-2056). If nodes are still
moving, an abortable frame is aborted (:2059-2063).

Knob values are not snapshotted. Renders read knobs live, under a hash taken earlier. See K1.

The request pass keys each context entry as `mixRenderContextKey(effect->getRenderHash(),
item.contextKey)` (ParallelRenderArgs.cpp:819). Input context args are keyed the same way
(:1683, :1708).

`renderRoI` takes `nodeHash` from the frame args (EffectInstanceRenderRoI.cpp:379). With no TLS it
builds args from the live `getHash()` and logs "[BUG]" (EffectInstanceRenderRoI.cpp:349, :358).
The hash of an input is `inputFrameArgs->nodeHash`, or the live `input->getHash()` when that input
has no frame args (EffectInstanceRenderRoI.cpp:1139).

### M178 context key

The `RenderContext` hash is a ContentHasher over its sorted entries (RenderContext.cpp:279-296), so
it is deterministic. `mixRenderContextKey` (RenderContext.h:266, RenderContext.cpp:395) leaves the
hash unchanged when the key is 0. `restrictContextToNode` (ParallelRenderArgs.cpp:1504) narrows a
context to what the node's upstream reads (`getUpstreamContextReadSet` NodeInputs.cpp:2101, a
Tarjan walk memoized per `getRenderContextReadsGeneration()`). Every hash mark moves that
generation (`invalidateRenderContextReads()` Node.cpp:1415, :1442). Tested by
`Tests/RenderContextRequest_Test.cpp:366` (ContextKeyIsMixedIntoTheHash) and
`Tests/RenderContext_Test.cpp:106`, `:350`.

Expression results that read context variables are cached under (time, view, contextKey)
(`_exprCtxRes`, KnobImpl.h:499-505). Other results are cached by time only (KnobImpl.h:512), and
`getValue` evaluates at `ViewIdx(0)` (KnobImpl.h:762, :881). See P2.T3.

**M71** adds a `RenderContextBinding` value whose identity hashes the owner node's fully qualified
name, the knob name, the knob content hash, the variable name and the captured context hash. A
rename therefore changes the context key of any context that carries a binding. M71 also re-keys
`FrameValueMap` to (time, view) (its Knob.h diff), and ParallelRenderArgs visits an identity hop
under the identity view and the single input context. These change keys only through the context
key and the expression results, not through `computeContentHash`.

### Project digest versus frame range

`Project::computeContentDigestWithoutFileStamps` (Project.cpp:3504) covers format, PAR, fps, views,
OCIO source, working colorspace, layers and concatenation. It leaves out the frame range (comment
at :3518-3519) and GPU rendering (:3544-3545). `getContentDigest` (Project.cpp:3460) adds the OCIO
file stamp when the source is a file. The default config is a built-in URI (Project.cpp:1458).

Only nodes that read the frame range hash it (step 8). A frame range edit calls
`rehashNodesUsingFrameRange()` (Project.cpp:2896, defined :4082), which marks those nodes.
`Node::dependsOnProjectFrameRange()` (Node.cpp:1050) is true for any knob with an expression,
render-hashed or not. The overrides of `EffectInstance::dependsOnProjectFrameRange()`
(EffectInstance.h:1886, default false) that return true: OfxEffectInstance.h:198,
DiskCacheNode.cpp:206, NativeWrite.cpp:1698, USDWrite.cpp:554.

### Stability across sessions and AppImage mounts

- Native plug-ins stamp "native", `NATRON_VERSION_STRING`, `GIT_COMMIT` and the running
  executable's stamp (`Plugin::getBinaryContentStamp` Plugin.cpp:349). `runningExecutableStamp`
  (Plugin.cpp:67) hashes the mtime and size of `/proc/self/exe` (:71-72), not its path, so a
  remounted AppImage keys the same. NatronEngine is a static library (`Engine/CMakeLists.txt:219`), so the
  stamp covers engine code. If `statx` fails, a per-process `sessionSalt()` is used instead
  (Plugin.cpp:74).
- OFX plug-ins hash the binary's file name, mtime and size, not its path
  (`Plugin::ofxBinaryStamp` Plugin.cpp:332). Tested by `Tests/HashInvariants_Test.cpp:339`, `:352`.
- File knobs stamp absolute path, mtime, size and inode (`computeStamp` FileStampRegistry.cpp:41).
  A moved project or moved media misses the cache. That is safe.
- Unstable by design (`ContentHasher::sessionSalt()` ContentHasher.cpp:152, random per process):
  - the volatile plug-ins (`kVolatilePlugins` Plugin.cpp:95, salted at :369)
  - expressions containing a token in `kVolatileExpressionTokens` (Knob.cpp:6729-6731, salted at
    :6801)
  - expressions reading a metadata source whose revision is non-zero (Knob.cpp:6829)
- Same graph, same hash across rebuild, save/load and a second app: `Tests/HashInvariants_Test.cpp:230`,
  `:242`, `:260`, `:289`.

## Invalidated by

- **A knob edit.** `KnobHolder::bumpKnobsContentGeneration` (Knob.cpp:7500) calls
  `onKnobContentChanged`, which for a node is `Node::onKnobContentChanged` (Node.cpp:1731). That
  calls `markHashDirtyInternal`. A group's disable and lifetime knobs set `includeGroupContents`.
  `EffectInstance::onSignificantEvaluateAboutToBeCalled` (EffectInstance.cpp:3544) also ends in `node->markHashDirty()`
  (EffectInstance.cpp:3567).
- **Expression listeners.** `KnobHelper::invalidateContentHash` (Knob.cpp:6915) walks a knob's
  listeners to their holders' `onKnobContentChanged`. Listeners are not in `getHashDependents`.
- **Propagation.** A mark bumps the node's generation and queues it (Node.cpp:1407-1435). The
  queue is flushed when a hash is read (`flushHashDirtyMarksBeforeRead` Node.cpp:1457, which skips
  the walk when the reader is the only node queued). `flushHashDirtyMarks` (Node.cpp:1488) walks
  `getHashDependents` (Node.cpp:1074): outputs through group redirection
  (`appendOutputsWithGroupRedirection` NodeInputs.cpp:213), the outputs of nodes that
  `isSkippedByHashInputs` (Node.cpp:212), the roto paint tree, and metadata readers
  (`getMetadataReaders` Node.cpp:1790).
- **Where the walk stops.** It stops at a node that is already dirty and not queued
  (Node.cpp:1526-1528). The reasoning: anything that folds in a dirty node is already dirty.
- **Recompute.** `refreshDirtyHashes` (Node.cpp:1649) is a post-order walk over dirty inputs.
  `recomputeHashIfDirty` (Node.cpp:1592) leaves a node dirty while any of its inputs is dirty. It
  calls `effect->onNodeHashChanged` only when the value changed (EffectInstance.cpp:6174). That
  re-points the ActionsCache, moves the metadata generation, and clears expression results when
  `mayHaveKnobExpressions()`. Old entries are kept, so reverting an edit hits them again.
- **Project and Settings.** `Node::invalidateAllHashes` (Node.cpp:1438) bumps the global epoch.
  Callers: Project.cpp:1781, :2257, :2456, :2554, :2587, :2954 (any project knob), :3266;
  ProjectPrivate.cpp:163; Settings.cpp:2303 (any Settings knob). A node rehashes, and its entries
  change, only if the digest it folds in moved.
- **Topology.** See [topology.md](topology.md).

## Not invalidated by

- Hash-exempt knobs: hideInputs, caching, preview, the Python callback knobs, nodeInfos and label
  (`setHashExempt` at Node.cpp:3118, :3131, :3146, :3243, :3261, :3269, :3286, :3302, :3315, :3327,
  :3340, :3363, :3610; KnobTypes.cpp:1665). Covered by `Tests/hash_exempt_allowlist.txt` and
  `Tests/HashPluginSweep_Test.cpp:397`.
- Knobs that are neither persistent nor evaluate-on-change (`isRenderHashed` Knob.cpp:4228).
- Node position and label (`Tests/HashInvariants_Test.cpp:586`, `:603`).
- A rename of a node without expressions (step 4).
- The project frame range, for nodes that don't read it (step 8).
- GPU on or off (Project.cpp:3544-3545).
- Settings knobs, unless one feeds the project digest. The only one a grep of render-time
  `appPTR->getCurrentSettings()` reads finds that affects pixels is transform concatenation, which
  the digest covers. Recorded as a grep result, not a proof.
- A file rewritten on disk, until Reload (`Tests/FileStamp_Test.cpp:332`). This is by design;
  see node-private.md (P2.T4).

## Covered by tests

- Seeded stress (`Tests/CacheStress.h`): every case compares cached renders bit-exactly with a reference from `clearAllCaches` plus `clearActionsCache`, in Legacy and in Task graph; `CacheStressConcurrentTest` covers edits during a render.
| Test file | What it pins |
| --- | --- |
| `Tests/HashInvariants_Test.cpp:230-773` | Identical graphs, reverts, worker-thread edits, keys, expressions, connect/swap, reload, labels, viewer cached frames (:773). |
| `Tests/LazyHash_Test.cpp:259-659` | Lazy branch rehash, persistent non-evaluating knob (:384), expression listener outside the graph (:527), group disable (:612), GroupInput rewire (:659). |
| `Tests/NodeContentHash_Test.cpp:85-329` | Name independence, renames with/without expressions, diamonds, masters, cycles. |
| `Tests/KnobContentHash_Test.cpp:57-236` | Knob hash inputs and which knobs are render-hashed. |
| `Tests/HashPluginSweep_Test.cpp:362` | Every render-hashed knob of every plug-in moves the hash and its revert restores it. |
| `Tests/KnobUndoHash_Test.cpp:128` | Undo/redo restore hashes downstream. |
| `Tests/RotoContentHash_Test.cpp:93-255` | Roto context and stroke hashes. |
| `Tests/RenderContext_Test.cpp:95-350`, `Tests/RenderContextRequest_Test.cpp:277-733` | Context hash, read sets, context key mixing, per-context expression results. |
| `Tests/FileStamp_Test.cpp:146-400` | File stamps and reload. |
| `Tests/HashInvariants_Test.cpp:339`, `:352` | OFX binary stamp, mount-independent. |

Not found: a test that a project frame range edit re-keys a frame-range reader; that a project
format, OCIO or layer edit re-keys every node; that volatile expressions and plug-ins re-key per
session; and that a precomp's hash follows its pre-rendered output.

## Suspected holes

- **K1. Hash captured before a render, knobs read live during it.** `buildArgsMap` reads each node's
  hash once (ParallelRenderArgs.cpp:2004) and the render reads knob values live. An edit landing
  between the capture and the knob read stores pixels of the new state under the old key.
  `snapshotMetadata` (:1811) catches only edits visible in metadata, and only before the render.
  Later edits rely on the abort. But `ViewerInstance::markAllOnGoingRendersAsAborted`
  (ViewerInstance.cpp:2986) keeps the oldest render running in draft mode, during partial updates,
  or when asked (:2989, :3001). Tiles finished before an abort stay cached. This branch has no
  post-render store guard. **Fixed by M202**: `CleanHashSnapshot::allowsStoreUnder` gates the
  stores in renderRoI, deep, viewer, roto mask and ActionsCache, and retracts what was stored when
  the graph moved mid-render. Phase 242.3 should still stress it.
  Stress case: `CacheStressConcurrentTest.DISABLED_KnobEditsDuringRender`; in a background app `canSetValue` is true during a render, so the edit lands mid-render.
- **K2. No-TLS fallback to the live hash.** `getRenderHash()` (EffectInstance.cpp:539-541) and the
  input hash in renderRoI (EffectInstanceRenderRoI.cpp:1139) fall back to the live `getHash()`.
  Several callers pass this node's hash, or an input's live hash, where the input's render hash is
  meant:
  - `Node::makeInfoForInput` (Node.cpp:3026, :3032) passes `getHashValue()` of this node into the
    input's `getFrameRange_public` and `getRegionOfDefinition_public`.
  - DiskCacheNode.cpp:188 and DeepCrop.cpp:171 pass `input->getHash()`.
  - DeepCrop.cpp:134, DeepExpression.cpp:343, DeepRemoveLayers.cpp:234, DeepFromImage.cpp:110,
    DeepReformat.cpp:172, DeepMerge.cpp:417, DeepRecolor.cpp:138 and DeepAddLayers.cpp:218 forward
    their own `hash` to an input.

  The ActionsCache then stores the input's answer under a hash that isn't the input's.
  **Fixed by M202** for makeInfoForInput, DiskCacheNode and the deep nodes (they switch to
  `input->getRenderHash()`). See actions-and-metadata.md.
- **K3. Pixel-affecting knobs outside the hash.** A knob that is neither persistent nor
  evaluate-on-change is not hashed (Knob.cpp:4228). The plug-in sweep (HashPluginSweep_Test.cpp:362)
  checks only knobs that are render-hashed, so it can't catch a pixel-affecting knob that isn't.
  No instance is known. **Checked in M260:** `CacheStressTest.NonHashedKnobSweep` renders with each
  non-hashed knob changed and compares against a fresh build.
- **K4. Expression dependencies are what the parser finds. Fixed in M260:** knob reads made while
  an expression runs are recorded (`ExpressionReadRecorder`) and registered as dependencies, at set
  time and on each evaluation (queued to the main thread from render threads), and a new one marks
  the hash. Tests: `NodeContentHashTest.AnExpressionReadingANodeFoundByGetattrDependsOnIt`; the
  random walk has `dynamicExpression`. A read the expression has not yet made (a branch never
  taken) is still unknown. The analysis below describes the code before the fix.
  A Python expression that reaches
  another node dynamically (built names, `getattr`) has no listener and does not fold the target's
  hash (`getContentHashDependencies` Knob.cpp:7117). Volatility comes from a fixed token list
  (Knob.cpp:6729-6731). Within a session a volatile expression hashes the same every time, since
  the salt is per process (:6801), so its node is cached like any other. Cross-reference P2.T3.
- **K5. `appendNodeContentHash` is opt-in.** State a node keeps outside its knobs is keyed only if
  the node overrides it and marks the hash on change (EffectInstance.h:403). Four nodes do. P2.T4
  audits the rest.
- **K6. Frame range for non-render-hashed expressions.** `Node::dependsOnProjectFrameRange`
  (Node.cpp:1050) counts any knob with an expression, but `computeContentHash` folds the range only
  when a render-hashed knob has one (Node.cpp:1035). Such a node is marked on a range edit without
  its key changing. That costs a recompute; it is not a staleness bug. **Fixed in M260:** only
  render-hashed expressions count. Test:
  `NodeContentHashTest.ARangeEditDoesNotMarkANodeWhoseOnlyExpressionIsOnANonHashedKnob`.
- **K7. Settings outside the digest.** `invalidateAllHashes` (Settings.cpp:2303) only bumps the
  epoch. A Settings knob that affects pixels without feeding the digest would not re-key anything.
  The grep found none (see "Not invalidated by"). **Fixed in M260:** render-affecting Settings
  (transform concatenation) feed the project digest. Tests:
  `NodeContentHashTest.ARenderAffectingSettingMovesTheProjectDigest`,
  `CacheStressTest.RenderSettingEdit`.
- **K8. Session stability.** `statx` failing on `/proc/self/exe` (Plugin.cpp:72-74) makes every
  native hash per-session. That is safe, because it only causes misses. No persistent cache
  (disk cache, image-caches.md) can then be reused.
