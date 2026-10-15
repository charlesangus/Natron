# M100 - Content Node Hashing

Replace the `knobsAge` counter hash with a content hash. Identical state gives an identical
hash, so undo and A/B toggling reuse the cache. Any change to a knob, the topology or an
upstream file changes the hash. The change lands in steps: shadow hash plus audit, then file
identity, then switching the cache keys, then deleting the age machinery. Depends on
M93 - Serialization Boundary, because Phases 100.4 and 100.5 touch `Node.cpp` load code and
`NodeSerialization`. M16 - Project Format Redesign deletes the serialized `KnobsAge` field,
as it already plans to.

**Absorbs M97 - Value-Based Node Hashing** (cancelled 2026-10-12, user): its prior art and its
Write read-back acceptance now live here (Phase 100.7). Prior art: branch
`m76/hash-gated-readback-wip` (`f1fbe003a`) has a working value-based content hash
(`EffectInstance::computeContentHash`, `Engine/EffectInstanceContentHash.cpp`) and the Write's
hash-gated, EXR-only read back on top of it, with the hash stored as `natron:outputHash` in the
EXR header and tests (`Tests/Native/NativeWriteReadBack_Test.cpp`). Its Xvfb check passed the
full undo-restores-the-match flow. Mine it for Phases 100.1–100.2, but don't merge it as is: it
sits on pre-review M76 - Native Write code.

## Notes (investigation 2026-10-11, code reading only; line numbers re-checked 2026-10-14)

Current line numbers (2026-10-14): computeHashInternal `Node.cpp:765-861`, purge call `:856` (guard `:851`), helper `removeAllImagesFromCacheWithMatchingIDAndDifferentKey` `:905` (loading guard `:914`, `:936`); `Hash64.cpp:43`; knobsAge load `Node.cpp:1458`, save `ProjectSerializationConvert.cpp:158`; creation date `ProjectPrivate.cpp:131`; cache lookup equality `Cache.h:1025`; `isMT` guard `EffectInstance.cpp:3433` (bump `:3442`); knobsAgeChanged slaving `Node.cpp:4907/4913`; `U32 newAge` `Node.cpp:1609`; paste/clone bump `NodeGraphPrivate10.cpp:257`; ActionsCache limit `EffectInstancePrivate.cpp:455` (`hardwareIdealThreadCount() * 2`); NATRON_CACHE_VERSION 9 at `GlobalDefines.h:97`. P5.T1 age hacks: reader filename `Node.cpp:5267`, group-disable loop `:5689`, layer-user bump `:6214`, group-input force `NodeInputs.cpp:861`, format loop `Project.cpp:2338`, restore defaults `EffectInstance.cpp:5669`. `Tests/BaseTest.cpp:365-380` asserts the hash is path-dependent; P4.T1 rewrites it.


**Current scheme:**
- `computeHashInternal` hashes the following (`Node.cpp:762-865`), via vector + boost CRC-64 (`Hash64.cpp:42`):
  - the node's age;
  - the IO container's age;
  - inputs as `hash + i`;
  - the script name;
  - the project creation time.
- When the hash changes, the old entries are purged (`:860`), except while a project is loading or closing (`:929`).
- `knobsAge` and `CreationDate` are saved in the project and restored on load (`Node.cpp:1679`, `ProjectPrivate.cpp:125`).
- The viewer and disk cache tables of contents persist across sessions (`Project.cpp:638`, `AppManager.cpp:501`).
- `ImageKey::operator==` ignores the holder (`ImageKey.cpp:82-98`). Cache lookup is by hash plus equality (`Cache.h:1702`), so entries are effectively shared already.

**Bugs in the current scheme.** Found by reading the code; none is reproduced yet, and the Phase 100.6 regressions must cover them:
1. **Stale images after reopen or autosave recovery.** An unsaved edit takes the age from 10 to 11, and its renders persist in the viewer and disk caches. On reopen the age is back at 10. A *different* edit reaches 11 again, which gives the same hash, so the stale pixels are served.
2. **Two copies of one project collide.** Save As keeps the creation date. Once the two copies diverge, equal ages give equal hashes for different content, in two windows or in later sessions.
3. **A file changed on disk stays stale.** No mtime reaches the hash; only Reload fixes it.
4. **Off-thread edits don't move the hash.** Knob edits made off the main thread, or during inputChanged, never bump the age (the `isMT` guard, `EffectInstance.cpp:3430`).
5. **Undo and A/B toggling discard valid cache.** Every return to an earlier state gets a new age.

Minor: inputs are combined with `+i`; `U32 newAge` truncates (`Node.cpp:1941`); paste and clone deliberately force different hashes (`Gui/NodeGraphPrivate10.cpp:249`).

## Design (applies to every task)

- **Node hash** = XXH3 over four parts. Keep `U64`, and never let the result be 0, because 0 means "not computed".
  1. **Identity:** plugin ID, plugin major and minor version, and a binary stamp (the `.ofx` path, mtime and size; for native nodes, the Natron version and git hash).
  2. **Render-hashed knobs**, sorted by script name, each as name plus content.
     - Content per view and dimension: the static value; the *whole* curve (keys, interpolation, tangents, extrapolation; weights after M96 - Weighted Curve Tangents); the expression string; parametric, table and string data.
     - Doubles are canonicalized: −0 becomes +0, and all NaNs become one value.
     - Each knob memoizes its hash behind a dirty bit.
     - Linked or slaved knobs hash the value they read through the link, which replaces the `knobsAgeChanged` slaving (`Node.cpp:5378`).
  3. **Inputs:** each index mixed with the input's hash, or a fixed "unconnected" marker. The viewer hashes its active inputs with their index. `getInput()` already applies group redirection.
  4. **Project digest:** formats, fps, frame range, OCIO config identity and working space, layer-registry content, and the render-affecting preferences.
- **Which knobs are render-hashed:** value knobs that are not hash-exempt and are persistent or evaluate on change. Erring towards including a knob is the safe direction, since the cost is only a cache miss.
  - Explicitly exempt: label, sublabel, preview, hide-inputs, info text and user colour.
  - Do NOT exempt a knob just because it doesn't evaluate on change: secret persistent OFX state params are often flagged that way.
- **What leaves the hash:** the script name and the creation time. One exception: a node with an expression also folds in its fully qualified name, because `thisNode` and `thisGroup` are resolved by name.
- **Expressions** fold in the content hashes of their dependency knobs (`getExpressionDependencies`, with a visited set). Tokens such as `random`, `time.`, `open(` and `os.` mark the knob volatile.
- **Volatile:** a per-session salt, reused within a session but never served from the disk cache in a later one. It applies to volatile expressions and to plugins on a "volatile" list (Text, which depends on fontconfig, etc.).
- **Roto and RotoPaint:** `RotoItem::appendContentHash`, memoized per item and dirtied wherever `incrementNodesAge()` is called today. It covers the item's knobs, control and feather points with their curves, stroke points, and the order of items. Each stroke's internal nodes fold in their own item's hash.
- **Read/Write containers:** the container's knobs are hashed into the embedded node. This replaces M58's age fold.
- **Files:**
  - A `FileStampRegistry` holds (path, statx mtime_ns, size, inode) with a short TTL.
  - Single files: `KnobFile`, the OCIO config, LUTs, fonts and DeepRead layers. A Write's output file is not stamped.
  - Sequences: a sequence stamp, the hash of every frame's stamp, taken from the existing directory scan. Rescan on inotify (`QFileSystemWatcher`), on app focus-in (NFS gets no inotify), and on Reload.
  - Per-render guard: stat each Read's current frame when render args are set up, and restart the render if the stamp moved. Keep this guard off the serial render path (M90 - Render Path Serial Overhead, M82 profiler).
- **Animation:** hash the whole curve. Time stays in `ImageKey` for frame-varying nodes, as today. Hashing the value at each frame is a later optimization that can ride the per-request hash of M71 - RFV Render Context.
- **Eviction:** no purge on hash change; LRU reclaims old entries. Explicit purges stay on node delete, Reload, Clear Cache and a cache-version change. Raise `ActionsCache`'s instance limit so toggling between two states keeps both.
- **Threading:** off-thread setters set an atomic dirty flag and post `mustComputeHashOnMainThread`. `getHashValue()` and the render-args snapshot recompute a dirty hash under the node lock. The main-thread assert goes away.
- **Persistence:** stop reading `KnobsAge` on load and write 0 until M16 - Project Format Redesign removes it. M93's golden harness already strips it. Bump `NATRON_CACHE_VERSION` when the keys switch over.
- **GPU vs CPU:** not part of the key. Their outputs differ by about a rounding step and already mix in the cache today.
- **Related milestones:**
  - M92 - Linked Nodes gets simpler if this lands first, since link groups hash their shared storage.
  - M19 - USD/Hydra Foundation's `stackHash` should reuse the knob hash.

## Phase 100.1: Hasher and knob content hash
- [x] M100.P1.T1 — Vendor XXH3 and add a streaming ContentHasher
  - files: libs/xxhash/xxhash.h, libs/CMakeLists.txt, Engine/ContentHasher.h, Engine/ContentHasher.cpp, Tests/ContentHasher_Test.cpp
  - approach: header-only XXH3-64 behind ContentHasher (appendU64, appendDouble with canonicalization, length-prefixed appendString and appendBytes; finalize never returns 0). First check whether the container's OIIO `hash.h` exposes XXH3; if it does, use it and skip vendoring.
  - verify: `ctest -R ContentHasher` covers known vectors, -0/+0 and NaN canonicalization, and ("ab","c") ≠ ("a","bc")
  - size: M
- [x] M100.P1.T2 — Add memoized KnobI::appendContentHash for every knob type
  - files: Engine/Knob.h, Engine/Knob.cpp, Engine/KnobImpl.h, Engine/KnobTypes.cpp, Engine/Curve.cpp
  - approach: for each view and dimension, hash the static value, the full curve and the expression string. Memoize behind a dirty bit that is cleared in onInternalValueChanged and on curve or expression changes. Linked dimensions hash through their master.
  - verify: Tests/KnobContentHash_Test.cpp: every kind of mutation changes the hash, reverting restores it, and a slaved knob hashes equal to its master
  - size: L
- [x] M100.P1.T3 — Classify render-hashed knobs and exempt UI-only ones
  - files: Engine/Knob.h, Engine/Knob.cpp, Engine/Node.cpp, Engine/OfxParamInstance.cpp
  - approach: add the new KnobI API setHashExempt()/isRenderHashed() (neither exists yet). isRenderHashed() = value type && !hashExempt && (persistent || evaluateOnChange). Call setHashExempt on label, sublabel, preview, hide-inputs, info text and user colour.
  - verify: a test shows that editing the label or info text leaves the hash unchanged, and editing a secret persistent param changes it
  - size: M

## Phase 100.2: Node content hash in shadow
- [x] M100.P2.T1 — Compute the Node content hash beside the age
  - files: Engine/Node.cpp, Engine/Node.h, Engine/NodePrivate.h, Engine/Project.cpp, Engine/Project.h
  - approach: identity + sorted hashed knobs + inputs by index (mixed, with an unconnected marker) + the project digest, stored as contentHash. getHashValue is unchanged.
  - verify: Tests/NodeContentHash_Test.cpp: two freshly built identical graphs hash equal; topology changes and input swaps produce different hashes
  - size: M
- [x] M100.P2.T2 — Hash roto/paint items and IO containers
  - files: Engine/RotoItem.cpp, Engine/RotoDrawableItem.cpp, Engine/Bezier.cpp, Engine/RotoStrokeItem.cpp, Engine/RotoUndoCommand.cpp, Engine/Node.cpp
  - approach: a per-item memoized appendContentHash, dirtied wherever incrementNodesAge is called (RotoDrawableItem::incrementNodesAge at :651; callers in Bezier.cpp ×15, RotoUndoCommand.cpp ×8, RotoItem.cpp:189, RotoStrokeItem.cpp). Stroke internal nodes fold in their item's hash, and the container's knobs fold into the embedded IO node.
  - verify: moving one control point changes the hash and undo restores it; a micro-benchmark measures hash cost on a 5k-stroke comp
  - size: L
- [x] M100.P2.T3 — Fold expression dependencies into knob hashes
  - files: Engine/Knob.cpp, Engine/Knob.h, Engine/Node.cpp
  - approach: fold in each dependency knob's content hash (visited set to stop cycles), the owner's fully qualified name when the node has expressions, and a session salt when a volatile token appears.
  - verify: a test shows that changing a referenced knob on a node that is not upstream changes the hash, and that an expression cycle terminates
  - size: M
- [ ] M100.P2.T4 — Add a NATRON_HASH_AUDIT shadow cross-check
  - files: Engine/Node.cpp, Engine/Node.h, Tests/CMakeLists.txt
  - approach: log once per knob whenever the age and content hashes disagree, in either direction. Run the full ctest with the audit on and record what it finds in `## Decisions`.
  - verify: an audit ctest run in which every discrepancy is explained or fixed
  - size: M

## Phase 100.3: File identity
- [x] M100.P3.T1 — Add FileStampRegistry and single-file stamps
  - files: Engine/FileStampRegistry.h, Engine/FileStampRegistry.cpp, Engine/KnobFile.cpp, Engine/Project.cpp, Tests/FileStamp_Test.cpp
  - approach: statx stamps with a TTL memo. The hashes of KnobFile and the OCIO config include the stamp, which is re-validated at render start.
  - verify: touching the file changes the hash; an unchanged file keeps a stable hash
  - size: M
- [ ] M100.P3.T2 — Add sequence stamps with inotify and focus-in rescan
  - files: Engine/FileStampRegistry.cpp, Engine/Nodes/IO/NativeRead.cpp, Engine/Nodes/IO/ReadTimeDomain.cpp, Engine/Nodes/Deep/DeepRead.cpp, Engine/PrecompNode.cpp, Gui/GuiApplicationManager.cpp
  - approach: hash all frame stamps from the existing directory scan (SequenceParsing via KnobFile). NativeRead's Reload knob is at NativeRead.cpp:186, its purge at :586; NativeWrite has its own purgeCaches/Reload too. The only existing QFileSystemWatcher is in FileSystemModel (file dialog), so this adds the first engine-side watcher. Watch the directory with QFileSystemWatcher, rescan on app focus-in, and force a rescan on Reload.
  - verify: rewriting one frame of a sequence changes the Read's hash; confirm the viewer re-renders in a GUI test or UAT
  - size: L
- [x] M100.P3.T3 — Add a per-render current-frame stat guard
  - files: Engine/ParallelRenderArgs.cpp, Engine/FileStampRegistry.cpp, Engine/ViewerInstance.cpp
  - approach: stat each Read's frame file when render args are set up. On a mismatch, update the registry, recompute the hash, abort and restart the render.
  - verify: a test overwrites a frame between two renders and gets the new pixels; the M82 profiler shows no serial regression
  - size: M

## Phase 100.4: Switch keys and drop the purge
- [ ] M100.P4.T1 — Make getHashValue return the content hash and bump the cache version
  - files: Engine/Node.cpp, Global/GlobalDefines.h, Engine/Hash64.cpp, Tests/BaseTest.cpp
  - approach: remove the script name (except on nodes with expressions), the creation time and the age from the key. Set NATRON_CACHE_VERSION to 10.
  - verify: the existing hash tests pass; two identical nodes share one cache entry (a counting effect renders once)
  - size: M
- [ ] M100.P4.T2 — Replace purge-on-hash-change with LRU eviction
  - files: Engine/Node.cpp, Engine/EffectInstancePrivate.cpp, Tests/DeepRenderPipeline_Test.cpp, Tests/DeepImageCache_Test.cpp
  - approach: remove the purge in computeHashInternal and keep the delete, Reload and Clear Cache purges. Raise the ActionsCache instance limit. Rewrite the tests that assert a purge as tests that assert reuse.
  - verify: going value 1 → 2 → 1 causes zero re-renders on the third step
  - size: M
- [ ] M100.P4.T3 — Make the hash recompute lazy and thread-safe
  - files: Engine/Node.cpp, Engine/Node.h, Engine/EffectInstance.cpp, Engine/Knob.cpp
  - approach: an atomic dirty flag that any thread can set. getHashValue and the render snapshot recompute under the node lock. Remove the main-thread assert; edits during inputChanged mark the node dirty.
  - verify: a test sets a knob from a worker thread and the next render sees the new hash; run TSan if it is available
  - size: L

## Phase 100.5: Remove knobsAge
- [ ] M100.P5.T1 — Turn core age bumps into dirty marks and delete the age hacks
  - files: Engine/Node.cpp, Engine/NodeInputs.cpp, Engine/EffectInstance.cpp, Engine/Project.cpp, Engine/Node.h
  - approach: incrementKnobsAge becomes markHashDirty. Delete the knobsAgeChanged slaving, the group-input force, the group-disable loop, the format loop and the layer-user bump.
  - verify: full ctest, plus disabled-Group and GroupInput rewire tests
  - size: L
- [ ] M100.P5.T2 — Migrate the remaining age-bump callers
  - files: Engine/RotoDrawableItem.cpp, Engine/RotoStrokeItem.cpp, Engine/RotoUndoCommand.cpp, Engine/Nodes/Channel/AddLayers.cpp, Engine/Nodes/Deep/DeepAddLayers.cpp, Gui/NodeGraphPrivate10.cpp, Gui/MultiInstancePanel.cpp, plus the test callers Tests/DeepReadWrite_Test.cpp:974, Tests/DeepRenderPipeline_Test.cpp:855,1275, Tests/DeepChannelNodes_Test.cpp:894-896
  - approach: apply the same replacement (AddLayers.cpp:404, DeepAddLayers.cpp:376, NodeGraphPrivate10.cpp:257, MultiInstancePanel.cpp:1612), and delete the bump that forces paste and clone to hash differently. Tests that bump or read the age switch to markHashDirty/getHashValue.
  - verify: `grep incrementKnobsAge` finds nothing; ctest passes
  - size: M
- [ ] M100.P5.T3 — Stop reading KnobsAge on load
  - files: Engine/Node.cpp (:1458 setKnobsAge on load), Engine/ProjectSerializationConvert.cpp (:158 save), Serialization/NodeSerialization.h (:101), Serialization/BoostXml/NodeXml.h (:127, :219)
  - approach: ignore the field on load and write 0; keep the struct field and XML I/O so the format is unchanged. M16 - Project Format Redesign deletes it.
  - verify: the M93 golden harness is unchanged; load → save → load gives an identical hash
  - size: M

## Phase 100.6: Invariant tests and cost
- [ ] M100.P6.T1 — Add a hash-invariant and regression suite
  - files: Tests/HashInvariants_Test.cpp, Tests/CMakeLists.txt
  - approach: the same state gives the same hash (fresh build, save/load, a second app instance); every edit changes it; undo restores it; topology changes and file touches change it; label and position changes don't. Add regressions for Notes bugs 1, 2 and 4.
  - verify: `ctest -R HashInvariants`
  - size: M
- [ ] M100.P6.T2 — Add a plugin sweep: every render-hashed knob of every bundled plugin changes the hash
  - files: Tests/HashPluginSweep_Test.cpp, Tests/CMakeLists.txt, Tests/hash_exempt_allowlist.txt
  - approach: create each plugin from appPTR's list. For each isRenderHashed knob, perturb it by type, assert the hash changed, revert, and assert it is restored. The allowlist explains every exemption.
  - verify: ctest passes with the OFX bundle
  - size: L
- [ ] M100.P6.T3 — Benchmark hash cost
  - files: tools/bench/hash_bench.py
  - approach: time the hash recompute on the bench comps and on a heavy roto comp. Target: keystroke to rehash under 1 ms on HD comps.
  - verify: record the results in `## Decisions`
  - size: M

## Phase 100.7: Write read back gated by the hash
- [ ] M100.P7.T1 — Read back an EXR frame only when its stored hash matches the node
  - files: Engine/Nodes/IO/NativeWrite.{h,cpp}, Engine/EffectInstance.h, Tests/Native/NativeWriteReadBack_Test.cpp, Tests/gui/m76_write.py, Engine/Nodes/README.md
  - approach: the shipped NativeWrite already has a file-driven read back (readBackLayers(), readBackActive() gated on the readBack knob, a Reload button, and a secret non-persistent Read Back Stamp knob `kNatronWriteParamReadBackStamp` that pokes the age-based hash). Port only the hash-gate half of `f1fbe003a` (origin/m76/hash-gated-readback-wip) onto it, using the Phase 100.4 content hash: the Write stores its node's hash as `natron:outputHash` in each EXR it writes; read back shows the file only when the stored hash equals the current hash, and shows the input otherwise; offered only for EXR. Delete the Read Back Stamp knob once the content hash makes it redundant. The test and GUI script exist on main: extend them, don't recreate. This replaces M76.P3.T2, which was cancelled.
  - verify: `NativeWriteReadBack_Test` covers match, mismatch after an edit, and a restored match after undo; the GUI script repeats the undo flow under Xvfb.
  - size: M

**Verification gate:** full ctest green in both scheduler modes; the HashInvariants and HashPluginSweep suites pass; the audit run leaves no discrepancy unexplained; the regressions for bugs 1, 2 and 4 pass; value toggling and undo hit the cache; touching a frame file refreshes the viewer in the AppImage; `incrementKnobsAge`/`knobsAge` appear nowhere outside serialization. Renaming a node or reopening the project in another session leaves its hash unchanged; the Write reads back an EXR only on a hash match (M100.P7.T1).

## Decisions

- 2026-10-12 — **Absorbed M97 - Value-Based Node Hashing** (user): M97 duplicated this milestone, and its blocker (the scope call) was settled here. Its prior art and the Write EXR read back moved into the intro and Phase 100.7.
- 2026-10-14 — **Freshness check at claim**: P3.T2, P5.T2, P5.T3 and P7.T1 had stale `files`/approach and were rewritten in place (NativeWrite already has a file-driven read back; KnobsAge lives in M93's `Serialization/` struct). P5.T3 re-sized S→M. Notes line numbers refreshed.
- 2026-10-14 — **Overlap hold**: M100.P4.T3, M100.P5.T1 and M100.P7.T1 touch `Engine/EffectInstance.{h,cpp}`, which M90 - Render Path Serial Overhead (earlier on the board) also edits in M90.P2.T2. Those three tasks wait until M90 merges; everything else proceeds.
- 2026-10-14 — **P2.T1 deviations** (code: 3dad48a52): unconnected inputs are skipped rather than marked (each connected input hashes with its index, then a connected-input count), so undoing a Merge connect restores the hash even though Merge keeps the new empty A slot. Only the active output format is hashed, not the format list. The project digest is computed on demand, not memoized. Open for P2.T4's audit: a digest-only change (OCIO, layer registry) doesn't trigger a recompute until the next age bump; frame range in the digest grows when a Read is added (`unionFrameRangeWith`), which would re-key every node, so it may need dropping; `GIT_COMMIT` in `Global/GitVersion.h` is a committed constant, so native code changes without a version bump keep the same stamp.
- 2026-10-14 — **P2.T3** (code: e82e1975c): the dependency fold isn't memoized (only a knob's own content is), so it is never stale at the knob level. At the node level, editing a knob that another node's expression reads doesn't yet recompute that node's shadow `getContentHash()`; P4.T3's lazy dirty-flag recompute must mark expression listeners' nodes dirty too.
- 2026-10-14 — **P3.T2 gap** (code: b63c1dfd2): a watcher event or focus-in only clears the stamp registry; nothing recomputes the affected nodes' hashes or re-renders the viewer, and in-place content writes that don't change mtime-visible attributes are caught only by focus-in or Reload. Close it in P4.T3 (lazy hash): the registry notifies listeners, and nodes whose file knobs match the invalidated directory mark themselves dirty and request a re-render. The viewer-refresh half of P3.T2's verify waits for that.
- 2026-10-14 — **P3.T3** (code: f51cfcc93): the guard adds one virtual, `EffectInstance::refreshInputFilesForRender`, to `Engine/EffectInstance.h`. That is a one-line, region-disjoint addition next to M90's `rendersOnlyInsideRenderWindow`, so it was not held behind M90. Limits: the proxy file isn't checked; an overwrite that changes resolution still needs Reload for metadata; the test bypasses the image cache, so the downstream purge is untested until Phase 100.4's reuse tests.
- 2026-10-14 — **NaN preference out of the project digest**: the NaN-handling preference is being removed app-wide (`DECISIONS/2026-10-14-drop-nan-check.md`, executed by M90 - Render Path Serial Overhead), so the digest stops hashing it.
- 2026-10-14 — **Phases 100.1–100.3 green** at e2a7b5d (remote run 38666840782): 2198/2199 in legacy and taskgraph, with every new suite passing (ContentHasher, KnobContentHash, NodeContentHash, RotoContentHash, FileStamp). The one failure is the new Tests_HashAudit aggregate run, which segfaults in its own per-node map partway through; a fix is in progress. P3.T2 stays open until the viewer-refresh half lands (P4.T3). Code: P1.T1 a2a600901, P1.T2 85ef62afd, P1.T3 9d8011db8, P2.T1 3dad48a52, P2.T2 bd38484fa + f21d85b68, P2.T3 e82e1975c, P2.T4 c89795b9b, P3.T1 f3ae86637, P3.T2 b63c1dfd2, P3.T3 f51cfcc93, build fixes ad1c147cf/e2cf55f4f/the include pin.
