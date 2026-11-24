# M569 - USDInline Node

A `USDInline` node takes a block of `usda` text and adds it to the scene as a layer, composed over the input. It is for the cases no node covers yet: a quick prim, an override a pipeline snippet describes, an experiment with composition arcs, or text pasted from a bug report. It mirrors Solaris Inline USD (`.local/docs/houdini/nodes/lop/inlineusd.md`): the text is parsed into an in-memory layer which "adds it to the layer stack in this node's output", i.e. as the strongest layer. In Natron that is the node's own stack entry, authored by the same path every Scene node uses: `emitSceneLayer` (`Engine/Nodes/Scene/SceneNodeAuthoring.h`, as in `USDVariant::renderScene`), with the author function calling `SdfLayer::ImportFromString` on `edit.layer`. A parse failure is a node error that carries USD's own message and the line number.

The node registers under `PLUGIN_GROUP_USD_Edit` (or the submenu `PLUGIN_GROUP_USD_Composition` if that fits better when implementing; the toolbar decision record is `DECISIONS/2026-11-22-usd-toolbar-submenus.md`), one Scene input and one Scene output.

## Scope

**In:**
- A multi-line `usdText` knob, label "USD Source" (`KnobString::setAsMultiLine`, plain text, monospaced where the knob GUI allows), holding `#usda 1.0` text. The default is a minimal valid layer (`#usda 1.0` and a blank line) that composes nothing.
- Authoring: `emitSceneLayer`; the author function runs `edit.layer->ImportFromString(text)`. The imported layer is the node's entry, strongest of the stack at that point (strength matches Solaris, where the new layer goes on top of the input's stack).
- Errors: if `ImportFromString` fails, a `TfErrorMark` around the call captures USD's diagnostics; the node fails with the first message's text and its line number (`line 7: Syntax error...`), and the input scene is not passed through. An empty text, or text with only the header, is valid and passes the input through unchanged. Warnings (unknown metadata, unresolved asset paths in the text) are shown as a node warning, not an error.
- Time codes: when the text authors no `timeCodesPerSecond`, the layer gets the upstream's (`SceneNodeAuthoring::getTimeCodesPerSecond`), so time samples written as frame numbers land on those frames, as every other node's layer does. A text that authors its own `timeCodesPerSecond`, `upAxis` or `metersPerUnit` keeps them: the layer's root metadata is the user's (it flows to the payload through `readLayerMetadata`, so, like any explicit metadata, it is a hint and converts nothing; M241 - Up Axis And Units Policy).
- The layer's own `subLayers`, `references`, `payload` and `variants` in the text compose as written. Relative asset paths in the text are resolved against the project directory (or the working directory for an unsaved project), which is how other nodes anchor their file paths, because the node's layer is anonymous and has no directory of its own.
- Cache key: the text, the node's resolved project-directory anchor and `appendUpstreamMetadata`; the layer reads nothing of the input, so it is memoized independent of the input's prims (the USDVariant `SceneLayerReads` shape with `frameRange = false`).
- Node doc `USDInline.md`, tests, one Xvfb GUI script.

**Out:**
- Allow Following Nodes to Edit New Layer: every Natron node authors its own layer over its input, so a following node never edits this node's layer. No control.
- The snippets pop-up and `UsdInlineLayers.txt`: no milestone.
- Embedding expressions or variables in the text (Houdini expressions, VEX snippets, `@` names): none. Knob expressions on the text knob still work as for any string knob.
- A syntax-highlighting editor or in-place error markers in the text widget: none. The error message carries the line number; editing support is a UI milestone nobody has asked for.
- Reading `.usda` from a file: USDRead's job.
- Round-tripping the composed stage back to text (an export-as-text button): none.
- Pruning or protecting parts of the input from the text: composition is USD's.
- `#usdc` or other binary content in the knob: `ImportFromString` takes text only; a non-`#usda` header is an error.

**Edge cases:**
- Parse error with several diagnostics: the first one, with its line, is the message; the rest are appended to it as further lines in the tooltip-style detail of the persistent message (bounded to ten lines).
- Text declaring a `defaultPrim` or a prim at a path the input also defines: both compose by strength (the text wins where it authors an opinion). That is USD, not a warning.
- Text authoring `def Cube "box"` at a path where the input has a different type: USD composes the stronger type, as for any `def` over a `def`; no node warning.
- Text referencing a prim path outside the scene (`over "Nowhere"`): authored; it composes nothing and the viewer shows nothing, no warning (an `over` of a missing prim is valid USD).
- A reference or sublayer in the text to a file that does not exist: USD reports a composition error at stage open; the node does not run that check, and the existing stage-composition error path (Scene Graph or viewer error) reports it. The viewer never loads payloads without user action, and a `payload` arc in the text is unloaded until the user loads it, like any other.
- The upstream scene is empty: the node still outputs the text's layer, as USDRead and the primitive nodes do.
- Very large text: no cap (no hand-holding size caps); the parse runs inside `renderScene`, which checks for abort before and after (`ImportFromString` itself cannot be interrupted, so the text is expected to be human-scale).
- Editing the text while the viewer renders: the usual node invalidation cancels the render; the memo key changes.
- Name collisions with other nodes' prim names: none; prims are the text's.
- No backward compatibility: new node, nothing imported.

**Assumptions:**
- `SdfLayer::ImportFromString` on an anonymous layer replaces the whole content (including `edit.layer`'s metadata that `authorLayerMetadata` would set), so the node authors the project metadata only after the import for fields the text left unauthored. T1 verifies which of `upAxis`, `metersPerUnit` and `timeCodesPerSecond` survive.
- USD's parser diagnostics reach the caller through the Tf error stack with a line number in the message text (`Syntax error at line N`); T1 captures a real error to confirm the format before the line-number extraction is relied on. If it is not in the text, the node reports the message USD gives without a number and the milestone records that.
- The anonymous layer from `emitSceneLayer` can be written by `ImportFromString` before it is sealed against edits; if the layer is already sealed at that point, the author function imports into a scratch layer and `TransferContent`s into `edit.layer`.

## Phase 569.1: The node

- [ ] M569.P1.T1 — USD text parsing helper
  - files: `Engine/Nodes/Scene/USDInlineParse.{h,cpp}` (new, free of knobs), `Tests/Scene/USDInline_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: `bool importUsdText(const SdfLayerRefPtr& layer, const std::string& text, std::vector<std::string>* warnings, std::string* error, int* line)`: opens a `TfErrorMark`, calls `layer->ImportFromString(text)`, on failure collects the mark's errors into `error` (first message), extracts the line number from USD's message (a `line <n>` match), and returns false; on success returns the mark's warnings (`TfDiagnosticBase` status/warning entries) in `warnings`. Blank or header-only text succeeds with an empty layer.
  - verify: gtest: a valid prim parses and `layer->GetPrimAtPath` finds it; header-only text is empty and valid; a syntax error on line 3 reports line 3 and a non-empty message; text with a wrong header (`#usdc`) fails; a duplicate-prim text composes without error (parse-level).
  - size: M

- [ ] M569.P1.T2 — The USDInline node
  - files: `Engine/Nodes/Scene/USDInline.{h,cpp}`, `Engine/Nodes/Scene/USDInline.reg.inc`, `Engine/Nodes/Scene/CMakeLists.txt` (or the Scene node source list), `Gui/Resources/Images/NativeNodes/USDInline.png`, `Tests/Scene/USDInline_Test.cpp`
  - approach: a scene node on the USDVariant pattern: one Scene input, a multi-line `usdText` knob ("USD Source", default `#usda 1.0`), `renderScene` runs `emitSceneLayer` with an author function that calls `importUsdText` on `edit.layer`, then fills `timeCodesPerSecond`, `upAxis` and `metersPerUnit` from the edit where the text left them unauthored (per the Assumptions check), sets `SceneLayerReads` with a digest of the text, the anchor and the upstream metadata, `frameRange = false`. On failure `setPersistentMessage(eMessageTypeError, "line N: ...")` and return `eStatusFailed`; on warnings set a warning message; on success clear. Knob label is the readable script name, tooltip explains the format and that errors name the line. Register under `PLUGIN_GROUP_USD_Edit`.
  - verify: gtest on payloads: Sphere -> USDInline with `over "Sphere" { double radius = 5 }` composes `radius` 5 over the input's; text defining a new prim adds it; a syntax error fails the node with the line number in its message and no output; text that sets `metersPerUnit` is preserved in the payload's metadata; an empty text passes through; the same text twice hits the layer memo (no second parse).
  - size: L

- [ ] M569.P1.T3 — Strength and composition tests
  - files: `Tests/Scene/USDInline_Test.cpp`
  - approach: tests on the node as built: the text layer is the strongest entry of the output stack (a USDEditTransform or USDSetAttribute downstream still overrides it, per the rule that later nodes are stronger); text with a relative sublayer and a relative reference resolves against the project directory; a `payload` arc in the text is unloaded in the default load set and loads with the user's load set; a text prim over an upstream prim keeps the upstream's untouched attributes.
  - verify: the tests pass, together with every existing `USDVariant_Test` and `ScenePayload` test (no change to the payload code is expected).
  - size: M

## Phase 569.2: Docs and GUI check

- [ ] M569.P2.T1 — Node doc page
  - files: `Engine/Nodes/Scene/docs/USDInline.md`
  - approach: document the knob, that the text becomes the strongest layer at this node, what a parse error looks like (message with the line), how relative paths resolve, time-codes behaviour, and two short `usda` examples (a new prim; an `over` of an upstream prim). No mention of other applications.
  - verify: a reviewer runs both examples in the node; text and results agree.
  - size: S

- [ ] M569.P2.T2 — Xvfb GUI script
  - files: `Tests/gui/usd_inline.py`
  - approach: under Xvfb at 1920x1080 build Sphere -> USDInline from Python; set the text knob to an `over` of the sphere's radius and assert the composed value through the viewer-side stage query; set text with a syntax error and assert the node's message names the line; fix it and assert the message clears; save and reload the project and assert the text and the result persist. Screenshot of the node with the error shown.
  - verify: the script passes in the remote `--gui` gate; the user OKs the screenshot (one file).
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `USDInline_Test` passes; `Tests/gui/usd_inline.py` passes under Xvfb at 1920x1080; the user OKs the error screenshot.

## Decisions

- 2026-11-24 — **A USDInline node takes usda text as a layer composed over the input (user):** a text knob of usda imported as a layer (`SdfLayer::ImportFromString`), composed over the input, strength matched to Solaris Inline USD; parse errors become node errors with the line number.
- 2026-11-24 — Strength is "strongest layer at this node", as Solaris adds the layer to the top of the stack; Natron nodes each own their layer, so Solaris's "Allow Following Nodes to Edit New Layer" has no control (PM draft).
