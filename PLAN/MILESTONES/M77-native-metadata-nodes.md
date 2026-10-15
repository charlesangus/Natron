# M77 - Native Metadata Nodes

A small set of native nodes that edit the per-frame metadata from M74 - Native Metadata Core. Runs in `Engine/Nodes/Metadata/`.

## Phase 77.1: Nodes

All four nodes derive from `NativeEffectBase`, modelled on `Engine/Nodes/TypedPassthrough.cpp`: one image input, `isIdentity()` returns input 0, no `render()`. They override `deriveOutputMetadata()` (`ImageMetadata`, `Engine/Nodes/Metadata/ImageMetadata.h`), reading `getUpstreamMetadata()` / `getInputMetadata(n, ...)`. Registration: an include plus `registerBuiltInPlugin<T>(QString::fromUtf8(""), false, false)` in `AppManager::loadBuiltinNodePlugins()`; grouping `PLUGIN_GROUP_METADATA` in each `getNativePluginDescription()`. Tests go in `Tests/Native/` (globbed); `MetadataSourceTestEffect` (`Tests/NativeMetadataTestEffect.h`) and `Tests/NativeMetadataPolicy_Test.cpp` show the pattern.

- [ ] M77.P1.T1 — ModifyMetadata node
  - files: new `Engine/Nodes/Metadata/ModifyMetadata.h/.cpp`, `Engine/AppManager.cpp`, `Global/Macros.h` (add `PLUGIN_GROUP_METADATA "Metadata"` beside the other `PLUGIN_GROUP_*`), new `Tests/Native/NativeModifyMetadata_Test.cpp`
  - approach: 8 fixed operation slots (`KnobTable` packs rows into one string, so cells can't take expressions). Each slot is a `KnobGroup` with an op choice (none / set / remove / rename), a key `KnobString`, a rename-to `KnobString`, a type choice (int / double / string) and one value knob per type, so values animate and take expressions. `deriveOutputMetadata()` starts from upstream and applies the slots in order.
  - verify: the gtest on `MetadataSource` → ModifyMetadata covers set, remove and rename; unrelated keys survive; a keyframed or expression value differs between frames 1 and 2; output metadata is recomputed after a knob edit.
  - size: M

- [ ] M77.P1.T2 — CopyMetadata and RemoveMetadata nodes
  - files: new `Engine/Nodes/Metadata/CopyMetadata.h/.cpp`, `RemoveMetadata.h/.cpp`, `MetadataKeyFilter.h/.cpp` (shared glob matching), `Engine/AppManager.cpp`, new `Tests/Native/NativeCopyMetadata_Test.cpp`, `Tests/Native/NativeRemoveMetadata_Test.cpp`
  - approach: CopyMetadata has inputs A (pixels, identity) and optional B (metadata via `getInputMetadata(1, ...)`). Key filter is a comma/newline-separated glob list in a `KnobString` (empty = all). Modes: replace (keep only filtered B keys), merge-B-wins (`ImageMetadata::merge(..., eMergePreferOther)` onto A), merge-A-wins (`eMergePreferThis`). B disconnected → A's metadata passes through. RemoveMetadata: a "remove all" `KnobBool` plus a glob list.
  - verify: gtests with two `MetadataSource` nodes: each mode, filter on/off, disconnected B, a glob such as `exr/*`.
  - size: M

- [ ] M77.P1.T3 — ViewMetadata node
  - files: new `Engine/Nodes/Metadata/ViewMetadata.h/.cpp`, `Engine/AppManager.cpp`, new `Tests/Native/NativeViewMetadata_Test.cpp`
  - approach: a passthrough with a `Metadata` page holding a multi-line HTML `KnobString` built like `Node::createInfoPage()` (`Engine/Node.cpp`: not animated, not persistent, multi-line, custom HTML, not evaluate-on-change) showing an HTML-escaped key/value table of `getOutputMetadata(currentFrame)`. Refresh on a Refresh button, on time change, and on metadata refresh / hash change; find the right hooks, and make sure a time-driven refresh doesn't mark the project dirty or trigger a render. No custom Qt widget.
  - verify: gtest that the text holds every key and value and that `ofx/frame` follows the frame; Xvfb GUI check (Read an EXR with custom attributes, add the node, see the table, change the frame), screenshot shared with the user before sign-off.
  - size: M

- [ ] M77.P1.T4 — Help text and docs
  - files: `Engine/Nodes/Metadata/{Modify,Copy,Remove,View}Metadata.cpp` (`description` strings only), `Engine/Nodes/README.md`, `Tests/Native/NativePluginList_Test.cpp` (or a new test beside it)
  - approach: one-paragraph help per node; a README "Metadata" section listing the key prefix convention (`ofx/`, `exr/`, `exif/`, `tiff/`, `dpx/`, `cin/`, `oiio/`) and the Write routing rules as implemented in `OiioWriteSupport::applyMetadata()`, plus the slot-based UI rationale.
  - verify: a gtest that four plugins have grouping `Metadata` and non-empty descriptions; README prefixes match `applyMetadata()`.
  - size: M

**Verification gate:** all four nodes covered by gtests; Xvfb GUI check with screenshot approved by the user; CI green.

## Decisions

- 2026-10-15 — **Freshness check at claim**: all four briefs rewritten in place. Base is `NativeEffectBase` with `isIdentity()` (as `TypedPassthrough`), not `NativeImageEffect`; `Gui/InfoViewerWidget.cpp` is the viewer's colour readout, so ViewMetadata uses an info-page-style HTML `KnobString`; menu grouping is set per node and needs a new `PLUGIN_GROUP_METADATA`, so it moved into T1–T3; T4 re-sized S→M.
- 2026-10-15 — **ModifyMetadata uses 8 fixed operation slots** (PM, on the consultant's advice): `KnobTable` can't hold per-cell expressions, and per-frame values were the point. A real dynamic table is a later milestone if wanted.
- 2026-10-15 — **Claimed with known small overlap** (user): `Engine/Nodes/README.md` (M100 - Content Node Hashing) and the `AppManager.cpp` registration list; resolved at rebase rather than waiting.
