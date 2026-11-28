# M646 - Template Node And Versioning

The template workflow redesign that came out of F33 - Graph Diff And Merge's UAT round 1
(2026-11-27). Today a template is any project file. "Update from Template" only ever re-reads
the path that was imported. A v002 the supervisor rebuilt, or made by importing v001 and
re-exporting it, has fresh node IDs, so the three-way merge matches nothing and re-adds every
node. This milestone makes the template a first-class object:
- a **template node** carries the template's identity, name and version;
- **Define Template** and **Publish Template** write numbered versions;
- **Update to Latest** and **Update from File…** run the three-way merge against a newer
  version, with base = the version that was imported;
- a loop-style hull shows the template's members in the graph.

It is a member of F33 - Graph Diff And Merge: it branches off
`feature/f33-graph-diff-and-merge` and merges into it. M647 - Template Compare Layout runs
beside it.

Code facts (diagnosis of 2026-11-27, on the F33 branch tip e073795254):
- A template entry is `TemplateImportSerialization {key, path, contentHash, importedAt, base}`
  (`Serialization/ProjectSerialization.h:86-92`). The key comes from the file name
  (`templateKeyStem`, `Engine/ProjectTemplateImport.cpp:58-72`).
- Base↔theirs matching is `matchNodes(base, theirs, "")` (`Serialization/Merge/ThreeWayMerge.cpp:593`).
  With an empty key, `collectClaims` claims by theirs' own node ID and ignores theirs'
  `origin` (`Serialization/Merge/NodeMatcher.cpp:69-71`). `adoptByStructure` can't start
  without a matched neighbour (`NodeMatcher.cpp:340-362`).
- Opening a file and saving it under a new name keeps node IDs (`Engine/NodeName.cpp:110-111,202-206`).
  Import re-mints them and keeps the old ID only as `origin.nodeId` (`ProjectTemplateImport.cpp:126-139`).
- The GUI update leaves the path empty unless the stored file is missing (`Gui/Gui60.cpp:101-110`).
  `ProjectMerge::updateFromTemplate` falls back to `entry.path` (`Engine/ProjectMerge.cpp:595`).
  With no changes, nothing is applied and nothing is said (`ProjectMerge.cpp:646-648`,
  `Gui60.cpp:116`).
- The context-menu update needs exactly one selected node that has an origin
  (`Gui/NodeGraph35.cpp:637-652`). After an import, several nodes are selected, so the action
  is missing. This is the likely cause of "Update does nothing" on a second template.
- The loop hull painter is `LoopBlockItem` in `Gui/LoopBlockOverlay.h/.cpp`, scheduled from
  `Gui/NodeGraph.cpp:162,342`. Built-in nodes register in `Engine/AppManager.cpp:2204`
  (`registerBuiltInPlugin<Backdrop>`).

## Scope

**In:**
- An unwired, non-rendering **Template** node. Its knobs: template name, template ID (read
  only), version, imported version, publish folder, and a newer-version indicator. Its buttons:
  Publish, Update to Latest, Update from File…, Compare with Base and Detach. It owns a hull
  drawn around its members.
- **Define Template** from the selection, or from the whole script when nothing is selected.
- **Membership:** nodes whose origin names the template, artist nodes wired strictly between
  them (MatchedEnclosed), and explicit Add to Template and Remove from Template.
- **Publish Template** writes `<name>_v<NNN>.ntpz`, the next number after the newest one in
  the publish folder. It never overwrites, and the published file contains the template node.
  Anyone may publish, including from an imported copy. Members' IDs are written as their
  `origin.nodeId`, so a re-import followed by a publish keeps the template's identity.
- **Import Template** recreates the template node from the file. A plain script with no
  template node gets a new template ID at version 1.
- **Update to Latest** picks the highest version with this template ID in the publish
  folder. **Update from File…** picks any file. Both run a three-way merge with base = the
  stored imported version.
- Base↔theirs matching through theirs' `origin.nodeId`, plus a low-match guard.
- Messages: "up to date" when an update changes nothing, and a warning with Merge Anyway when
  the template ID doesn't match.
- Deleting the template node detaches the template, as one undo step.
- Python bindings for define, publish and update-to-latest.
- Updated user docs.

**Out:**
- Ghost placement, the template offset and the "moved" state in Compare views (M647 - Template Compare Layout).
- Roles or permissions on who may publish (none).
- Opening or reading payloads during the newer-version check. It reads only the template
  node out of each candidate bundle's `project.yaml`.

**Edge cases:**
- The next version's file already exists, for example because someone else published it.
  Publish refuses and suggests updating to it first.
- Update from File… picks a file whose template ID is different or missing. A warning offers
  Cancel or Merge Anyway, and Merge Anyway still runs behind the low-match guard.
- Fewer than 25% of base's nodes match theirs (with base non-empty). The update refuses and
  names the counts, instead of re-adding every node.
- The template node is deleted. Members stay, their origins and the stored base are dropped,
  the hull goes away, and one undo restores it all. To delete the members, select the hull,
  which selects them all.
- The same template is imported twice. Each import has its own key and template node. The
  origin fallback only claims IDs that exist in that import's base.
- Remove from Template clears the node's origin and records it as excluded, so the
  MatchedEnclosed rule never pulls it back in.
- Add to Template on a node with no origin tags it as a new member, with no template node ID
  until the next publish.
- An update to a version older than the imported one is allowed, and the version knob shows
  what was merged.
- Update when the file is unchanged applies nothing. It says the template is up to date,
  and the artist's edits stay.

**Assumptions:**
- Scanning the publish folder for `*_v<NNN>.ntpz` and reading only each bundle's template
  node is fast enough to run when a project loads and when the node's panel opens, off the
  UI thread.

## Phase 646.1: Engine identity, publish and update

- [x] M646.P1.T1 — Template identity in the template entry
  - files: `Serialization/ProjectSerialization.h`, the YAML encoder and decoder for `TemplateImportSerialization` (grep the type under `Serialization/Yaml/`), `Serialization/Bundle/ProjectBundle.cpp`, `Tests/Serialization/ProjectBundleRoundTrip_Test.cpp`
  - approach: add `templateId` (a UUID string), `name`, `version` (int; 0 means defined but not yet published), `publishDir` and `excluded` (node IDs removed from the template by hand) to `TemplateImportSerialization`. Encode them in the project YAML next to the existing fields. No compatibility path: an entry without `templateId` fails to load as a template entry (the fork makes no backward-compatibility promise).
  - verify: compiled at P1.T9. A round-trip gtest saves and reloads an entry with every new field set, and the fields compare equal.
  - size: M

- [x] M646.P1.T2 — Template node
  - files: new `Engine/TemplateNode.h`, new `Engine/TemplateNode.cpp`, `Engine/AppManager.cpp` (register it beside `Backdrop`), `Engine/CMakeLists.txt`
  - approach: a built-in node with no inputs and no output that never renders, modelled on `Engine/Backdrop.cpp`. Its knobs, with labels matching the script names:
    - `templateKey`: hidden, names the template entry.
    - `templateName`.
    - `templateId`: read only.
    - `version`: read only.
    - `importedVersion`: read only.
    - `publishDir`: a path.
    - `newerVersion`: read-only text, empty when current.
    - Buttons `publish`, `updateToLatest`, `updateFromFile`, `compareWithBase`, `detach`. Each forwards to an `AppInstance` virtual that `GuiAppInstance` implements in P3.T2; find how an existing Engine button reaches the GUI and follow that pattern.

    Name/version knob edits write through to the entry. Exclude this plugin ID from `MergeScope` and from node matching, so the template node is never a merge member.
  - verify: compiled at P1.T9. A gtest creates the node, sets `templateKey` to an entry, and reads the entry's name and version back through the knobs.
  - size: M

- [x] M646.P1.T3 — Define Template and explicit membership
  - files: new `Engine/ProjectTemplateDefine.cpp`, `Engine/Project.h`, `Serialization/Merge/MergeScope.cpp`, new `Tests/Serialization/TemplateDefine_Test.cpp`
  - approach:
    - `Project::defineTemplate(nodes, name, group)` (all of the group's nodes when `nodes` is empty) mints a template ID and a key, then creates an entry at version 0 whose base is a snapshot of the members. It sets each member's `origin` to `{key, node: own ID}` and creates a template node above the members' bounding box.
    - `addToTemplate(key, node)` sets `origin` to `{key, node: own ID}` and removes the node from `excluded`.
    - `removeFromTemplate(key, node)` clears `origin` and appends the node's ID to `excluded`.
    - `MergeScope` drops `excluded` IDs from the MatchedEnclosed set.

    Each returns the graph ops it made, so the GUI can wrap them in undo.
  - verify: compiled at P1.T9. gtests:
    - define over a selection tags exactly those nodes;
    - define with nothing selected tags the whole group;
    - a removed artist node wired between members stays out of the merge scope;
    - an added node is in scope.
  - size: M

- [x] M646.P1.T4 — Import reads or mints the template node
  - files: `Engine/ProjectTemplateImport.cpp`, `Tests/Serialization/TemplateImport_Test.cpp`
  - approach: when the imported file holds a template node, take its `templateId`, `templateName` and `version`. Do not import it as a member: create a fresh template node in the target group, offset like the members, with `importedVersion = version` and `publishDir` = the file's directory. A file with no template node gets a new ID, name = the file stem, and version 1. Store the base without the template node.
  - verify: compiled at P1.T9. gtests:
    - importing a published file keeps its ID and version and creates exactly one template node;
    - importing a plain script mints an ID at version 1;
    - importing the same file twice gives two keys, two template nodes and the same template ID.
  - size: M

- [x] M646.P1.T5 — Base↔theirs origin fallback and low-match guard
  - files: `Serialization/Merge/NodeMatcher.cpp`, `Serialization/Merge/ThreeWayMerge.cpp`, `Serialization/Merge/ThreeWayMerge.h`, `Tests/Serialization/NodeMatcher_Test.cpp`, `Tests/Serialization/ThreeWayMerge_Test.cpp`
  - approach:
    - When matching base to theirs, an unclaimed theirs node whose `origin.nodeId` exists in base and is still unclaimed claims that base node. Do this before structural adoption, so adoption has seeds.
    - After matching, if base is non-empty and fewer than 25% of its nodes match, return an error result ("v002 shares N of M nodes with the imported base"); never return a merged snapshot.
    - Keep `IdPlusAdoption` scores on the existing corpus unchanged.
  - verify: compiled at P1.T9. gtests:
    - theirs made by importing base and re-exporting (fresh IDs, origins pointing at base) matches every node;
    - a fully rebuilt theirs with no shared IDs hits the guard;
    - the NodeMatcher corpus still scores 310/310.
  - size: M

- [x] M646.P1.T6 — Publish Template
  - files: new `Engine/ProjectTemplatePublish.cpp`, `Engine/Project.h`, new `Tests/Serialization/TemplatePublish_Test.cpp`
  - approach: `Project::publishTemplate(key, error)` scans `publishDir` for `<name>_v<NNN>.ntpz` and writes the next number. It refuses if that file exists or the directory is not writable. The written bundle holds:
    - the members, each node's ID replaced by its `origin.nodeId` when that origin names this key, else the node's own ID (that ID is also written back as the live node's `origin.nodeId`);
    - a template node with the new version and the same template ID.

    Afterwards, set the entry's base to the published member snapshot and both `version` and `importedVersion` to the new number. Re-saving an unchanged project then gives an empty diff against the published file.
  - verify: compiled at P1.T9. gtests:
    - publishing a defined template writes `_v001`, and again writes `_v002`;
    - publishing when `_v002` already exists refuses and writes nothing;
    - a publish from an imported copy keeps the original node IDs (a merge of v001 → v002 matches every node);
    - the written file opens as a project.
  - size: L

- [x] M646.P1.T7 — Update to Latest, Update from File and status
  - files: `Engine/ProjectMerge.h`, `Engine/ProjectMerge.cpp`, `Tests/ProjectMerge_Test.cpp`
  - approach:
    - Add `findLatestVersion(dir, templateId)`. It reads only the template node out of each candidate bundle's `project.yaml`, and is callable off the UI thread.
    - `updateFromTemplate` takes an explicit path. It reports a status: `updated`, `upToDate` (no ops and no new conflicts), `templateMismatch` (the file's template ID differs or is missing), `lowMatch` (from P1.T5) or `error`. A mismatch returns before merging unless the caller passes `allowMismatch`.
    - On success, update the entry's path, base, `importedVersion` and `version`, and the template node's knobs.
  - verify: compiled at P1.T9. gtests:
    - v001 imported, artist edits, v002 published from a fresh import of v001; update to latest gives a three-way result keeping the artist's edits and v002's changes;
    - updating with an unchanged file returns `upToDate`;
    - a different template's file returns `templateMismatch`, and with `allowMismatch` it merges or hits `lowMatch`.
  - size: M

- [x] M646.P1.T8 — Detach Template
  - files: `Engine/ProjectTemplateDefine.cpp`, `Engine/Project.h`, `Tests/Serialization/TemplateDefine_Test.cpp`
  - approach: `Project::detachTemplate(key)` returns the ops that clear every member's origin, remove the entry with its `templates/<key>/base.yaml` member and its `merge/conflicts.yaml` entries for that key, and delete the template node. Their inverse restores everything. Deleting a template node by any route goes through this, so the GUI undo is one step.
  - verify: compiled at P1.T9. A gtest detaches, checks that no member has an origin and that the entry and base member are gone after save, then applies the inverse ops and checks that the project equals the pre-detach snapshot.
  - size: M

- [x] M646.P1.T9 — Phase 646.1 build and test checkpoint (df1184c1ea; m646-gate1 green)
  - files: none
  - approach: syntax-check the touched TUs first (`tools/ci/local/syntax-check.py`), then run the remote gate build and ctest per the project's build tooling.
  - verify: green. The build is required.
  - size: M

## Phase 646.2: GUI, Python and docs

- [x] M646.P2.T1 — Template hull
  - files: `Gui/LoopBlockOverlay.h`, `Gui/LoopBlockOverlay.cpp`, `Gui/NodeGraph.cpp`
  - approach: extend the overlay to paint one block per template node, reusing `LoopBlockItem`'s hull and pan/zoom handling: a hull around the template node and its members (MatchedEnclosed plus explicit membership, minus `excluded`), in the template node's colour. Leave out the wedges and rings. Clicking the hull selects the members and the template node. The hull refreshes when membership or positions change.
  - verify: compiled at P2.T6. Exercised by P2.T5: the hull encloses the members after an import, and the enclosed set updates after Remove from Template.
  - size: M

- [x] M646.P2.T2 — Template actions in menus, buttons and the node graph
  - files: `Gui/Gui20.cpp`, `Gui/Gui60.cpp`, `Gui/NodeGraph35.cpp`, `Gui/MergeConflictResolve.cpp`, `Gui/GuiAppInstance.cpp`, `Gui/GuiAppInstance.h`
  - approach:
    - Add File > Define Template….
    - Implement P1.T2's `AppInstance` virtuals, so the node's buttons run Publish, Update to Latest, Update from File… (a picker opening in `publishDir`), Compare with Base and Detach.
    - The node-graph context menu offers Update to Latest, Update from File…, Compare with Base, Add to Template and Remove from Template whenever every selected node belongs to one template, replacing the single-selected-node rule at `NodeGraph35.cpp:637-652`.
    - Every edit goes through `ApplyGraphOpsCommand` as one undo step. Deleting a template node runs Detach.
    - `templateMismatch` shows a confirmation with Cancel and Merge Anyway. `upToDate`, `lowMatch` and publish refusals report non-modally (the user avoids pop-ups).
    - Run the newer-version check off the UI thread when a project loads and when the node's panel opens, and set `newerVersion`.
  - verify: compiled at P2.T6. Exercised by P2.T5.
  - size: L

- [x] M646.P2.T3 — Python bindings for define, publish and update-to-latest
  - files: `Engine/PyAppInstance.h`, `Engine/PyAppInstance.cpp`, `Engine/typesystem_engine.xml`, `Gui/PyGuiApp.h`, `Gui/PyGuiApp.cpp`, the Gui typesystem, new `Tests/Serialization/PyTemplatePublish_Test.cpp`
  - approach:
    - `app.defineTemplate(nodes, name, group=None)` returns the template node.
    - `app.publishTemplate(key)` returns the written path.
    - `app.updateTemplateToLatest(key)` returns the status string; the existing `updateFromTemplate(key, path)` also returns the status.
    - Failures raise `RuntimeError` through the inject-code early return M512 - Python importTemplate Binding used (Shiboken swallows a null return otherwise).
    - Old gtest: no `GTEST_SKIP`, `INSTANTIATE_TEST_CASE_P` only, no `ASSERT_TRUE(ptr)`.
  - verify: compiled at P2.T6. The gtest defines, publishes twice, and checks both files exist; a publish onto an existing version raises `RuntimeError`.
  - size: M

- [x] M646.P2.T4 — Docs for the template workflow
  - files: `docs/graph-merge.md`
  - approach: rewrite the template section around the new workflow:
    1. the supervisor defines and publishes v001;
    2. the artist imports and edits;
    3. the supervisor publishes v002, from the same script or from a fresh import;
    4. the artist runs Update to Latest.

    Cover membership, the hull, detach, the mismatch warning and the low-match guard. No external product names.
  - verify: the doc describes every action named in P2.T2 and passes the comment/docs policy check.
  - size: M

- [x] M646.P2.T5 — GUI test of the versioned workflow
  - files: new `Tests/gui/m646_template_versions.py`, `Tests/gui/m97_template_merge.py`
  - approach: on the `Tests/gui/guitest.py` harness:
    1. define a template from a selection and publish v001;
    2. import it into a second project, check the hull and the template node, and edit knobs as the artist;
    3. in a third project, import v001, change it, and publish v002;
    4. in the artist project, run Update to Latest and check a clean three-way result;
    5. run Update to Latest again and see the up-to-date message;
    6. Update from File… with another template, cancel the mismatch warning, and check the graph is unchanged;
    7. Remove from Template, then Add to Template;
    8. delete the template node, check the origins are gone, then undo and check they are restored.

    Port `m97_template_merge.py` to the new actions.
  - verify: both scripts pass under `Tests/gui/run-gui-test.sh` on the phase build.
  - size: M

- [x] M646.P2.T6 — Phase 646.2 build, ctest and GUI checkpoint
  - files: none
  - approach: remote gate build with ctest and `--gui` for `m646_template_versions.py`, `m97_template_merge.py`, `m104_merge_conflicts.py` and `m104_diff_overlay.py`.
  - verify: green. The build is required.
  - size: M

**Verification gate:**
- Full ctest is green, including the NodeMatcher corpus at 310/310.
- The four template GUI scripts pass remotely.
- A v002 published from a fresh import of v001 merges three-way into an artist's copy with no re-added nodes.
- Update with an unchanged file reports "up to date".
- Deleting the template node is one undo step.
- PR CI into the F33 branch is green.

## Decisions

- 2026-11-27 — **The template is a first-class node (user, F33 UAT round 1 grill):** Define Template creates an unwired info node (name, ID, version, publish path) with a loop-style hull around the members, and the template actions move onto it. A Backdrop subclass would have made membership spatial. A pass-through node would have forced a wiring point that templates with several outputs don't have.
- 2026-11-27 — **Identity uses both the origin fallback and Publish (user):** base↔theirs matching also claims through `origin.nodeId`, with a low-match guard, and Publish Template stamps the template ID and version into the file. Code finding: only raw node IDs carried identity, so a v002 rebuilt or re-exported from an import re-added every node.
- 2026-11-27 — **Publish source is the selection or the whole script (user).** Versions are written automatically as `<name>_vNNN.ntpz` beside the last one and never overwritten (user).
- 2026-11-27 — **Membership is origin plus enclosed plus explicit add and remove (user).** Removal is recorded in `excluded`, so MatchedEnclosed doesn't pull a node back in (planner).
- 2026-11-27 — **Update offers Latest and Update from File… (user):** Latest is the highest vNNN with the same template ID, and the node flags a newer version. Picking a different or missing template ID warns and allows Merge Anyway, still behind the guard (user).
- 2026-11-27 — **Plain scripts still import, and get a new template ID at v1 (user).**
- 2026-11-27 — **Deleting the template node detaches it (user):** the members stay, origins and base are dropped, and it is one undo step.
- 2026-11-27 — **Anyone may publish the next version of the same ID, imports included (user).** That is how a supervisor publishes v002 from a fresh import of v001.
- 2026-11-27 — The low-match guard threshold is 25% of base's nodes (planner): low enough for heavy supervisor rewrites, and it still catches a file with no shared identity.
- 2026-11-27 — "Update does nothing" on a second template: no content-hash early return exists, and an unchanged file correctly applies nothing. The likely real cause is the context menu's single-selected-node rule. P2.T2 replaces that rule and adds the up-to-date message (code finding).
- 2026-11-27 — **Claimed despite a shared file with M351 - Shader Graph Presentation (user):** `Gui/NodeGraph.cpp` overlaps; claim now, resolve conflicts when merging.
- 2026-11-27 (PM) — Phase 646.1 T1-T8 in df1184c1ea; gate m646-gate1 (fast ctest + m97/m104 GUI). Implementer's open points (PM defaults, revisit in Phase 2/UAT): Gui60/PyGuiApp pass allowMismatch=true until Phase 2's mismatch UI; version and importedVersion hold the same number (spec doesn't distinguish); addToTemplate gives an added node its own ID as origin; republishing from an imported copy keeps member IDs but the Template node gets the shot's ID.
- 2026-11-28 — Phase 2 T1–T5 in d6872562d0; m646-gate2 running. Open: node.destroy() from Python bypasses Detach; undoing a Publish leaves the file (next publish numbers past it); Define Template names the node "template" and opens its panel rather than prompting.
- 2026-11-28 — Phase 2 verified: m646-gate2 (full ctest green, m97/m104 GUI) + m646-gate4 (m646_template_versions.py green after two script fixes: wrapper lifetime, hull click clear of the mask stub). PR #411 into F33; review round 1.
