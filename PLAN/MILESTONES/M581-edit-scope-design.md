# M581 - Edit Scope Design

Designs an Edit Scope: a group node marked as the edit target. Interactive edits create or update ordinary nodes inside it rather than hidden state. Those edits are gizmo drags, prim inspector edits, and Scene Graph panel toggles (visibility, active). They can then be diffed (M103 - Graph Diff And Merge), merged, shared in live sessions (F5) and undone like any graph edit. The milestone writes `docs/edit-scope.md` in the code repo, runs a test-only prototype spike, and ends with a list of follow-up implementation milestones written in the doc, not as board rows. Today, a 3D drag goes through `SceneTransformPolicyTarget::commit` (`Gui/SceneTransformTarget.{h,cpp}` ~:633), which follows the `Settings::Viewer3DTransformTargetEnum` preference (`Engine/Settings.h` ~:99): the owner node's knobs (or its Replace-mode USDEditTransform), the USDEditTransform whose panel is open, or a new USDEditTransform after the viewed node. Edits land as `SceneKnobEdit`s through `applySceneKnobEdits` in one undo step. Selections become nodes through `Gui/SceneSelectionActions.{h,cpp}` (M232 - Scene Selection To Nodes). The prim inspector is read-only (M333 - Prim Inspector, Out: "edits are nodes"). The Scene Graph panel has no visibility or active toggles; its draw states are viewer-only (M556 - Scene Graph Bulk Commands). The reference is Gaffer's EditScope (`.local/docs/gaffer/1.7.5.0/Reference/NodeReference/Gaffer/EditScope.html`: "a container that interactive tools may make nodes in as necessary"), with its viewer shortcuts that prune, hide and unhide "from current EditScope" (`Interface/ControlsAndShortcuts`) and the 1.6/1.7 release notes on in-place inspector edits. The Light Mixer is a node (M573 - USDLightMixer Node), so it does not depend on this milestone.

## Scope

**In:**
- `docs/edit-scope.md`, covering:
  - the audit of today's edit paths;
  - the reference study;
  - the proposed node model: the scope as a Group with scene in and out, and which processor node each edit kind uses (USDEditTransform for transforms, USDPrune Deactivate or Hide, or USDSetAttribute, for active and visibility; USDEditPrim or USDSetAttribute for inspector values);
  - the rules for finding or creating a processor inside the scope (one per prim or one per kind with a growing pattern, and where in the scope's chain it goes);
  - how edits are keyed at the current frame, and undo;
  - how the scope interacts with the transform-target preference;
  - warnings when an edit is overridden downstream of the scope, using the inspector's opinion provenance;
  - serialization (nodes only), graph diff, and live sessions;
  - the open design forks, each with options and trade-offs, for the user.
- A test-only prototype: a Group used as a scope, an `acquireTransformEdit`-style helper that finds or creates a USDEditTransform inside it for a prim, and one undo step. The spike file is deleted once its findings are in the doc.
- The follow-up milestone list in the doc: name, intent and dependencies for each.

**Out:**
- Shipping any scope node, preference, chooser or editor integration: the follow-up milestones in the doc.
- The Light Mixer: M573 - USDLightMixer Node is a node and does not use a scope.
- Deciding the open forks: the user decides them from the doc.

**Edge cases:**
- The scope is not upstream of the viewed node: the doc covers what each candidate rule does (refuse with a message, fall back to the transform-target preference, or edit anyway, unseen).
- A node downstream of the scope overrides the edit: the doc proposes the warning, drawn from `SceneOps::getPropertyOpinions`.
- Edits to prims inside instances or unloaded payloads: the doc states the rules (no authoring inside instance proxies; no payload loading to make an edit).
- Several prims in one drag: the doc says whether one processor per prim or one per drag.
- Animated edits: the doc covers keying at the current frame, as `SceneKnobEdit` does for animated dimensions.

**Assumptions:**
- A Natron Group with Input and Output nodes passes scene data like any scene node (kind-agnostic groups), so a scope needs no new graph machinery. The spike checks this.

## Phase 581.1: Survey

- [ ] M581.P1.T1 — Audit today's edit paths
  - files: `docs/edit-scope.md` (new)
  - approach: write the "Edit paths today" section. It covers each path that turns an interactive action into graph state, naming file and function: the three transform-target policies in `SceneTransformPolicyTarget::commit` and `findSceneTransformOwner`, create-node-from-selection in `SceneSelectionActions`, knob prim tools (M334 - Knob Prim Tools), the Scene Graph panel row menu and bulk commands (state that is viewer-only), and the read-only inspector. Record for each what it creates or edits and how it undoes.
  - verify: the section lists every caller of `applySceneKnobEdits` and every node-creating action in `Gui/SceneSelectionActions.cpp` (checked by grep in the PR description).
  - size: M

- [ ] M581.P1.T2 — Reference study
  - files: `docs/edit-scope.md`
  - approach: write the "Reference" section from the local docs: Gaffer's EditScope and its viewer and editor behaviour (prune, hide and unhide from the current scope, in-place inspector edits, the scope chooser), and Solaris's Edit LOP and viewport overrides in the session layer. Say what each does with edits made outside its scope's branch. The section is a design reference: these names stay out of code, tooltips and node docs.
  - verify: every claim cites the local doc file it comes from.
  - size: M

## Phase 581.2: Design

- [ ] M581.P2.T1 — Node model and processor rules
  - files: `docs/edit-scope.md`
  - approach: propose the scope as a Group subclass or a Group with a marker knob (with trade-offs), the processor node per edit kind, and the find-or-create rules: matching an existing processor by prim and kind, chain order inside the scope, pattern growth versus one node per prim, and what happens to a processor whose prim disappears. Cover undo (one step per gesture), keying at frame, serialization and graph-diff behaviour, live sessions, and the overridden-downstream warning.
  - verify: each edit kind named in `## Scope` (transform drag, inspector value, visibility toggle, active toggle) has a row stating its processor, its find-or-create rule and its undo.
  - size: M

- [ ] M581.P2.T2 — Open forks laid out for the user
  - files: `docs/edit-scope.md`
  - approach: an "Open questions" section, each fork with options, pros, cons and implications, and no choice made:
    - one scope per script, or several with a current one;
    - how the current scope is chosen (a per-viewer chooser, a project-wide setting, the scope whose panel is open, the nearest scope upstream of the viewed node);
    - what happens when the scope is not upstream of the viewed node;
    - whether the scope replaces or joins the transform-target preference.
  - verify: each fork has at least two options, each with pros, cons and implications, and none is marked decided; the PM sends the section to the user as the milestone's open questions.
  - size: M

## Phase 581.3: Prototype spike

- [ ] M581.P3.T1 — Test-only scope prototype
  - files: `Tests/Scene/EditScopeSpike_Test.cpp` (new, deleted in P4.T1), `Tests/CMakeLists.txt`
  - approach: in the test file only, build a Group with Input and Output between a USDCamera and a viewer-less USDMerge. A local `acquireTransformEdit(group, primPath)` finds a USDEditTransform in the group whose pattern names the prim, else creates one, chains it before the Output and sets its pattern. Then apply a `SceneKnobEdit` through `applySceneKnobEdits` with an `UndoHistory`, and check the composed transform downstream of the group. Measure nothing.
  - verify: the test builds and passes: the downstream world transform moves; a second acquire returns the same node; a second prim adds a second processor (or grows the pattern, as P2.T1 proposes); one undo removes both the edit and the created node; the group round-trips through serialization with the processor inside.
  - size: L

## Phase 581.4: Close

- [ ] M581.P4.T1 — Findings, follow-up list, spike removed
  - files: `docs/edit-scope.md`, `Tests/Scene/EditScopeSpike_Test.cpp` (deleted), `Tests/CMakeLists.txt`
  - approach: write the spike's findings into the doc and add "Follow-up milestones": for each, name, intent, dependencies and the forks it waits on. They cover the scope node, gizmo drags into the scope, Scene Graph visibility and active toggles through it, inspector value edits through it, the chooser UI, and the overridden-downstream warning. Delete the spike file and its CMake entry in the same change.
  - verify: the doc's follow-up list names dependencies for every entry; the spike file and its CMake line are gone; the release gate builds without it.
  - size: M

**Verification gate:** release gate green per `.local/build-mode`; `EditScopeSpike_Test` passed before its removal (result recorded in `## Decisions`); `docs/edit-scope.md` is merged with the audit, the reference study, the model, the open forks and the follow-up list; the user answers the open forks before any follow-up milestone is put on the board.

## Decisions

- 2026-11-24 — **Design milestone (user):** design a Gaffer-style Edit Scope first. It is a group node marked as the edit target, where gizmo drags (USDEditTransform handles), prim inspector edits and Scene Graph panel toggles (visibility, active) create or update nodes inside it rather than hidden state. Deliverables: a design doc under `docs/`, a prototype spike if useful, and follow-up milestones as text in the doc, not board rows.
- 2026-11-24 — The Light Mixer is a node (user, M573), so Edit Scope is not its dependency.
