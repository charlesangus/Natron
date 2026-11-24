# M565 - USDConfigureLayer Node

**Overlap with M449 - USD Layer Save Paths, which the request did not know about:** M449 (doing, branch `milestone/m449-usd-layer-save-paths`) already ships a node named `USDConfigureLayer` (`Engine/Nodes/Scene/USDConfigureLayer.{h,cpp}`) with, behind `AuthoredKnobSet` toggles, `defaultPrim`, `comment`, `startTime`, `endTime`, `timeCodesPerSecond`, `framesPerSecond`, `upAxis`, `metersPerUnit`, `colorConfiguration`, `colorManagementSystem`, `renderSettingsPrimPath` and a custom-data table, plus `appliesTo` and `savePath`. That node authors **no USD layer**: it turns its toggled fields into `SceneLayerRegionSettings`, which only USDWrite reads, when it writes the region's file. So today an explicit layer edit has no live effect: the viewer, downstream nodes and `SceneMetadata` never see it, and only USDWrite's output does. The request's node (explicit layer-metadata edits that win over project-derived values) is therefore not a second node: this milestone **extends M449's `USDConfigureLayer`** with the live half and the controls M449 left out, and does not add another node of that name. The user confirmed this shape (2026-11-24).

How metadata is derived today: `SceneOps::readLayerMetadata` (`Engine/Nodes/Scene/SceneOps.cpp`) takes a payload's `SceneMetadata` (`upAxis`, `metersPerUnit`, `defaultPrim`, `timeCodesPerSecond`, `renderSettingsPrimPath`) from the strongest layer that carries each root field, falling back to the weaker payload. Every node's layer authors the project's `upAxis` and `metersPerUnit` and the upstream's time codes per second through `SceneNodeAuthoring::authorLayerMetadata`, which is why the project's values win everywhere (M241 - Up Axis And Units Policy). USDWrite then writes `authorStageMetadata` from the final payload's metadata (`Engine/Nodes/Scene/USDWrite.cpp`, documented in `Engine/Nodes/Scene/docs/USDWrite.md`) and, per M449, lets a region that lands in the root file override those fields.

## Scope

**In:**
- Live effect: when any live-meaningful field is toggled, the node also emits one anonymous layer (`emitSceneLayer`, the USDVariant pattern) authoring those fields on its root: `upAxis`, `metersPerUnit`, `defaultPrim`, `timeCodesPerSecond` and `renderSettingsPrimPath` (the fields `readLayerMetadata` reads, so downstream `SceneMetadata` reflects them), plus `startTimeCode`, `endTimeCode`, `framesPerSecond`, `doc` (the Comment) and `customLayerData`. The layer authors the project-derived values first (`authorLayerMetadata`) and the explicit ones after, so **an explicit field is the strongest opinion for that field from this node on**, and untoggled fields keep whatever the input says.
- Precedence, stated once: node layer explicit field > the same field from any upstream layer > the project's up axis and meters per unit and the project frame rate. USDWrite needs no new rule for the live path, because its root metadata follows the payload; M449's region override still wins at write time for a region landing in the root file, and it now agrees with the live value unless a stronger node says otherwise.
- Interaction with M241: an explicit Up Axis or Meters Per Unit differing from the project's is a hint, never a conversion (as Solaris and USD treat them): nothing is rotated or scaled, and USDRead's Match Project Up Axis and Match Project Units still correct against the **project**, not against explicit values. The node shows a persistent warning naming the difference ("Layer says Z-up; the project is Y-up; nothing is converted").
- Interaction with F27 and M448 - USD Layer Break: the node's layer is a normal entry; above a break it is context and not written, but its metadata still reaches the root output, because stage metadata is authored from the composed payload (M448's Edge cases). In Preceding mode the layer is emitted before `claimPreceding`, so it is claimed into the region with the layers it configures.
- Stage Variables (a Solaris control M449 left out): a table of named, typed rows (the same row types and table knob as Custom Data) authored as the layer's `expressionVariables` dictionary, live and written by USDWrite on the file that receives the region.
- Params-to-knob audit against Solaris Configure Layer (below), the node doc `USDConfigureLayer.md` (M449 writes the page; this milestone extends it), ctest coverage and one Xvfb GUI script.

**Out** (Solaris Configure Layer controls, one by one):
- Save Path, Applies To: M449 owns them.
- Default Primitive, Comment, Start Time, End Time, Time Codes per Second, Frames per Second, Up Axis, Meters per Unit, Color Configuration, Color Management System, Render Settings, Custom Data: already M449's knobs; this milestone adds only their live layer (and the colour fields stay write-time only, since `SceneMetadata` carries no colour management and no Natron node reads it).
- Also Set Metadata on Stage Root Layer: inherent here, since the composed stage's root metadata always follows the payload; no control.
- Flatten Input (layers or stage): destructive flattening of the graph; no milestone.
- Start New Layer, Set Input Active Layer to Explicit: Natron nodes each author their own layer, and explicit layers are M449's regions; no control.
- Houdini's `/HoudiniLayerInfo` prim and output processors: none.
- Converting scenes to the explicit up axis or units: none (M241's Out).

**Edge cases:**
- No live-meaningful field toggled (only Save Path, Applies To and the colour fields, which no live reader uses): no layer is emitted and the input passes through, as M449's node does today, so existing region behaviour and caches are untouched.
- Explicit Default Prim naming a prim that does not exist in the composed scene: warning, still authored (USD allows it; the file would not open as a reference until the prim exists).
- Explicit Time Codes Per Second: authored on the node's layer only. It changes the layer's own sample offsets and the payload's `timeCodesPerSecond` for later nodes; it never rescales existing samples. A note in the tooltip says so.
- Explicit Meters Per Unit of zero or less: node error.
- Explicit Start Time after End Time: warning, authored as given.
- Stage Variable rows with a repeated or empty name, or text that does not parse for its type: node error naming the row.
- A second Configure Layer downstream with the same field: the stronger (later) node wins for that field only; the earlier node's other fields persist.
- USDMerge of branches that configure the same field differently: the merge's existing rule applies (metadata follows the strongest input, `mergeStacks`); nothing new.
- Time-varying knobs: layer metadata is not animated, so the metadata knobs are non-animatable and read as plain values (M449's `renderScene` reads some at `args.time`; T1 switches them to `getValue`).
- No backward compatibility: nothing imported from old projects.

**Assumptions:**
- `readLayerMetadata` needs only the root fields listed; layer-level `doc`, start/end time codes, `framesPerSecond` and `customLayerData` live on the layer but are not part of `SceneMetadata`, so they are visible to USDWrite and `usdview`-style tools through the composed root layer rather than through `SceneMetadata`.
- The pxr build supports layer `expressionVariables` (`SdfFieldKeys->ExpressionVariables`, USD 24.03 or newer). T4 checks the build's version first; if absent, Stage Variables move to Out and the task is dropped.
- Authoring `upAxis`/`metersPerUnit` explicitly on an interior layer does not disturb `SceneLayerMemo` keys, because project values are already in every layer's key and the explicit ones are hashed into this node's facts.

## Phase 565.1: Live layer

- [ ] M565.P1.T1 — Emit a metadata layer from USDConfigureLayer
  - files: `Engine/Nodes/Scene/USDConfigureLayer.cpp`, `Engine/Nodes/Scene/SceneNodeAuthoring.{h,cpp}` (a helper `authorExplicitLayerMetadata`), `Tests/Scene/SceneLayerRegion_Test.cpp` or `Tests/Scene/ConfigureLayerLive_Test.cpp` (new), `Tests/CMakeLists.txt`
  - approach: in `renderScene`, after collecting the toggled fields into `SceneLayerRegionSettings` as today, if any of upAxis, metersPerUnit, defaultPrim, timeCodesPerSecond, renderSettingsPrimPath, start/end time, framesPerSecond, comment or custom data is toggled, call `emitSceneLayer` with an author function that runs `authorLayerMetadata(edit)` and then authors the explicit fields (`SetField` for upAxis/metersPerUnit/renderSettingsPrimPath, `SetDefaultPrim`, `SetTimeCodesPerSecond`, `SetFramesPerSecond`, `SetStartTimeCode`, `SetEndTimeCode`, `SetDocumentation`, `SetCustomLayerData`). Hash the explicit values and `appendUpstreamMetadata` into the facts digest; frame range off. Emit the layer before the region operation (`claimPreceding`/`openRegion`/`setOpenRegionSettings`) so it joins the region. Leave the region settings as they are.
  - verify: gtest on in-memory payloads: Sphere -> ConfigureLayer(upAxis Z, metersPerUnit 0.01, defaultPrim) gives `output.getMetadata()` of Z, 0.01, the default prim, while the project's own values are still authored by the layers upstream; an untoggled field keeps the upstream value; a node with no live field emits no layer (payload unchanged, same stack hash); a second ConfigureLayer downstream overriding only `metersPerUnit` leaves the first's `upAxis`.
  - size: L

- [ ] M565.P1.T2 — USDWrite follows the live metadata
  - files: `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: confirm `authorStageMetadata` reads the payload's metadata (it does) and make the start/end time, frames per second, comment and custom layer data of an explicit node layer reach the written root: where a field is explicit on the final stack's strongest configuring layer, `authorStageMetadata` uses it instead of the project range (`startTime`, `endTime`, `framesPerSecond` parameters of `writeFrameRange`). M449's region override at write time stays and wins when a region lands in the root file; add a test that the two agree.
  - verify: tests: a graph with explicit upAxis Z, a comment and a start time of 10 writes those on the root file; the project's values appear when no node sets them; with a region landing in the root, the region's value wins and matches the live payload; an M448 break above the node still writes the metadata on the root.
  - size: M

- [ ] M565.P1.T3 — Warnings against the project, and checks
  - files: `Engine/Nodes/Scene/USDConfigureLayer.cpp`, `Tests/Scene/ConfigureLayerLive_Test.cpp`
  - approach: compose one persistent message: Up Axis or Meters Per Unit differing from the project's ("nothing is converted"), a Default Prim missing from the composed scene (query through the input's borrowed stage; no payload loading), Start after End; `Meters Per Unit <= 0` and bad Stage Variable rows are errors. Clear the message when none apply.
  - verify: gtest on the message text for each case and for a clean configuration; the Default Prim check does not load an unloaded payload (a payload-only default prim warns "not found in the loaded scene").
  - size: M

## Phase 565.2: Stage Variables

- [ ] M565.P2.T1 — Stage Variables table, live and written
  - files: `Engine/Nodes/Scene/USDConfigureLayer.{h,cpp}`, `Engine/Nodes/Scene/SceneOps.{h,cpp}` (`SceneLayerRegionSettings` gains `expressionVariables`), `Engine/Nodes/Scene/USDWrite.cpp`, `Tests/Scene/ConfigureLayerLive_Test.cpp`, `Tests/Scene/USDWrite_Test.cpp`
  - approach: check the pxr version for `SdfFieldKeys->ExpressionVariables` first; if the build lacks it, stop and report (Stage Variables move to Out). Add a `stageVariables` table knob (the M449 `KnobLayerCustomData` row type, label "Stage Variables"), parse rows with the shared custom-data parser into a `VtDictionary`, author it on the live layer (`layer->SetExpressionVariables`) and carry it in the region settings so USDWrite writes it on the region's file.
  - verify: gtest: a String row and an Integer row land in the layer's `GetExpressionVariables()`; the written root file carries them; an invalid row names itself in the error.
  - size: M

## Phase 565.3: Audit, docs, GUI

- [ ] M565.P3.T1 — Params-to-knob audit
  - files: `Tests/Scene/ConfigureLayerLive_Test.cpp`
  - approach: a table of every Solaris Configure Layer control (In, M449-owned and Out, as in Scope) against the built node's knobs: each In or M449-owned control has a knob with its documented script name and (except Save Path and Applies To) a companion toggle; each Out control has none. Failure names the control.
  - verify: the audit test passes.
  - size: M

- [ ] M565.P3.T2 — Docs
  - files: `Engine/Nodes/Scene/docs/USDConfigureLayer.md`, `Engine/Nodes/Scene/docs/USDWrite.md`
  - approach: extend M449's page with the live effect, the precedence rule, the project-warning, Stage Variables and the tooltip on Time Codes Per Second; add a paragraph to the USDWrite page saying explicit layer metadata wins over the project-derived values and how a region in the root file interacts. No mention of other applications.
  - verify: a reviewer checks the precedence text against the tests in P1.T1 and P1.T2.
  - size: M

- [ ] M565.P3.T3 — Xvfb GUI script
  - files: `Tests/gui/usd_configurelayer_live.py`
  - approach: under Xvfb at 1920x1080 build Sphere -> USDConfigureLayer -> USDWrite from Python, toggle Up Axis (by editing it) and Meters Per Unit, assert the viewer-side `SceneMetadata`/stage root fields and the warning text, render USDWrite and read the file's root metadata back, then save and reload the project and assert the toggles survive. Screenshot of the node's page with the warning.
  - verify: the script passes in the remote `--gui` gate; the user OKs the screenshot (one file).
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `ConfigureLayerLive_Test`, `SceneLayerRegion_Test` and `USDWrite_Test` (including the M448 and M449 cases) pass; `Tests/gui/usd_configurelayer_live.py` passes under Xvfb at 1920x1080; the user OKs the screenshot.

## Decisions

- 2026-11-24 — **Layer metadata gets an explicit-edit node (user):** "Shape A plus Configure Layer": USDConfigureLayer = explicit layer-metadata edits (start/end time code, time codes per second, frames per second, up axis, meters per unit, default prim, doc string, customLayerData), defined to win over the project-derived values USDWrite writes today and to interact with M241's policy and F27's layer authoring.
- 2026-11-24 — The node already exists in M449 with the same fields but no live effect; this milestone is written as its extension, not a second node (PM draft, confirmed by the user the same day).
- 2026-11-24 — An explicit Up Axis or Meters Per Unit is metadata only, never a conversion (PM draft; matches M241's policy and Solaris).
- 2026-11-24 — **Extend M449's USDConfigureLayer rather than add a node (user):** one node and one set of controls; M565 adds the live layer, the precedence rule, Stage Variables and the audit on top of it.
