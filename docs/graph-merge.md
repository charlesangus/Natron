# Diffing and merging Natron projects

`natron-merge` compares and merges project bundles (`.ntpz`) as node graphs rather than as
bytes. It needs nothing but itself: no Qt, Python or Natron install, so it runs on a build
farm, in CI or in a git hook.

## Commands

```
natron-merge diff [--yaml] <before> <after>
natron-merge merge [--scope template:<key>|all] <base> <mine> <theirs> -o <out>
natron-merge git-driver <base> <current> <other>
natron-merge textconv <file>
```

### diff

Prints what changed from `before` to `after`, one section per node:

```
~ KeyGrade (renamed from Grade1)
  multiply: default → 1.5
  input Source: Read1 → Blur2
+ Blur2 (Blur)
  input Source: KeyGrade
- Crop1 (Crop)
```

`+` is a node that only `after` has, `-` one that only `before` has, and `~` one both have
that changed; changes to the project's own settings come last, under `~ Project`. A project file
leaves out knobs at their defaults, so a knob one side leaves at its default shows as `default`. Nodes inside groups are shown by their dotted path (`Look.Blur1`). Only the
graph is compared: the save date, the Natron version that saved the file and the viewer and
timeline state are not. Re-saving an unchanged project therefore gives an empty diff.

`--yaml` prints the same changes as graph operations, the form scripts and the merge engine
use.

### merge

Merges two edits of a common ancestor and writes the result to `out`:

- `--scope all` (the default) merges two versions of the same project, for example two
  branches of a shot. Nodes are matched by their IDs, which survive renames, moves and
  re-saves.
- `--scope template:<key>` updates a template imported into `mine` as `<key>`: `base` is the
  template as it was imported, `theirs` its new version, and only the nodes that came from the
  template, plus anything the artist put between them, are merged. Template nodes the artist
  deleted and recreated are matched by their place in the graph. `mine` must have an import
  with that key, else the merge is an error. The import then records `theirs` as its base, with
  its path, content hash and the time of the merge.

Where both sides changed the same thing differently, the result keeps **mine** and records a
conflict. The conflicts are saved in the bundle (`merge/conflicts.yaml`), so a conflicted
project still opens normally, with the conflicts listed for resolving later. A later merge keeps
the conflicts saved earlier, by `mine` and, with `--scope all`, by `theirs`, while the node each
names is still in the result. A summary of the new conflicts is printed on stderr.

### git-driver and textconv

`git-driver` is a git merge driver: it merges `%O` (the ancestor), `%A` (the current branch)
and `%B` (the other branch) with scope `all`, writes the result into `%A` and reports a
conflict to git. `textconv` prints the project as YAML with nodes and knobs sorted by name,
so `git diff` and `git log -p` show readable changes.

### Exit status

| Status | `diff`               | `merge`, `git-driver`      |
|--------|----------------------|----------------------------|
| 0      | no differences       | clean merge                |
| 1      | the projects differ  | merged, conflicts recorded |
| 2      | error                | error, nothing written     |

## Setting up git

Tell git which files are Natron projects, in the repository's `.gitattributes`:

```
*.ntpz -text merge=natron diff=natron
```

Then register the driver, once per clone (or in `~/.gitconfig`):

```
git config merge.natron.name "Natron project merge"
git config merge.natron.driver "natron-merge git-driver %O %A %B"
git config diff.natron.textconv "natron-merge textconv"
```

`git merge`, `git rebase` and `git cherry-pick` now merge projects node by node. A merge with
conflicts stops like any other conflicted merge: open the project in Natron, resolve, save,
then `git add` it and continue. `git diff` shows project changes as YAML; for the per-node
summary, run `natron-merge diff` on two checkouts, for example:

```
git show main:shot.ntpz > /tmp/before.ntpz
natron-merge diff /tmp/before.ntpz shot.ntpz
```

## Templates and versions

A template is a part of a script that is shared between shots and updated over time: a grade
setup, a despill, a whole comp structure. A supervisor defines and publishes it; artists import
it into their shots and edit it there; when the supervisor publishes a new version, each shot
takes it with a three-way merge that keeps the artist's edits.

### The Template node

Every template in a script is shown by a **Template** node. It has no inputs or output and never
renders. Its knobs show the template's name, its template ID (which stays the same across all
its versions), the version the shot was last merged with or published as, the publish folder
and, when one exists, the path of a newer published version. Its buttons are Publish, Update to
Latest, Update from File..., Compare with Base and Detach.

A hull in the Template node's colour is drawn around the node and its members. Click the band
along the hull's edge to select all of them, for example to move them or delete them together.
Deeper inside the hull, clicks and rubber-band selections work as anywhere else in the graph.

### Membership

A node belongs to a template when:

- it came from the template, or was added to it in a publish;
- it was added by the artist between two members, wired below one and above another; or
- it was added with **Add to Template** from the node graph's right-click menu.

**Remove from Template** takes a node out: it stays in the shot, the hull no longer goes around
it, and the next publish leaves it out. Only the selected nodes are taken out, including nodes
the hull goes around only because they sit between members. Both entries appear in the
right-click menu when the selected nodes that belong to a template, as the hull counts them, all
belong to the same one, and each is one undo step.

### Workflow

1. **The supervisor defines the template.** Select the nodes and choose File > Define
   Template (with nothing selected, the template is made of the whole script). A Template node
   appears above the nodes and its settings open: set the template's name and its publish
   folder. Each new template gets a name of its own (`template`, `template_2`, ...) until you
   rename it.
2. **The supervisor publishes version 1.** Press Publish on the Template node. It writes
   `<name>_v001.ntpz` in the publish folder.
3. **The artist imports it.** File > Import Template... brings the nodes and the Template node
   into the shot. The artist edits it like any other part of the script: changes knobs, adds
   nodes, rewires.
4. **The supervisor publishes version 2,** either from the same script, or after importing the
   template into a fresh script. Versions are numbered by template ID, not by file name: Publish
   writes the version after the one the shot was last merged with or published as, under the
   template's current name, and renaming the template does not restart the numbering. Publish
   is not an undo step: a published file stays published.
5. **The artist updates.** Press Update to Latest on the Template node, or choose it from the
   right-click menu with any member selected. The newest version with the same template ID in
   the publish folder is merged into the shot:
   - nodes the artist added stay where they are, and nodes the supervisor added appear;
   - knobs only one side changed take that side's value;
   - knobs both changed keep the artist's value and are listed in the Merge Conflicts panel,
     where each can be switched to the template's value;
   - the whole update is one undo step.

   When the shot already has the newest version, nothing changes and the status bar says the
   template is up to date. When a newer version is published, the Template node shows its path.

Update from File... merges any file instead of the newest version, for example to go back to an
older one. Compare with Base shows what the shot changed since the version it was last merged
with or published as. Errors from both go to the status bar.

Publishing from a shot is allowed too. If the folder already holds that version or a newer one
of the template, because someone else published it in the meantime, Publish refuses and says
so: update to that version first, then publish. Publish also refuses, naming the clash, when
another template's versions in the folder use the same file name; rename the template or
publish it to another folder.

### Copying and pasting

A copied or duplicated Template node starts a new template: the pasted nodes that came from the
original become the members of the copy, under a name and template ID of their own, and the
original template is unchanged. Cut and paste moves the template instead: the pasted Template
node keeps the template, its name, ID and version, but saved merge conflicts are dropped.

### Safeguards

- **Another template.** When the file picked with Update from File... is a version of a
  different template, or is not a published template at all (none of its Template nodes has the
  template's ID), a warning asks whether to cancel or to merge anyway. Cancel leaves the shot as it was.
- **Too few matching nodes.** When fewer than a quarter of the nodes of the version the shot
  was last merged with are found in the new file, the update refuses and names the counts, instead of
  re-adding every node as new.

### Detaching

Detach, or deleting the Template node in any way (in the graph, with the group that holds it,
or from Python), makes the template's nodes ordinary nodes of the shot:
they stay, but they no longer belong to the template, the stored copy of the imported version is
dropped and the hull goes away. Later versions can then no longer be merged in. One undo brings
it all back. To delete the nodes as well, click the hull to select them all first.

### From Python

```python
tpl = app.defineTemplate([app.getNode("Grade1"), app.getNode("Blur1")], "grade")
key = tpl.getParam("templateKey").getValue()
tpl.getParam("publishDir").setValue("/shows/templates")
path = app.publishTemplate(key)          # /shows/templates/grade_v001.ntpz
status = app.updateTemplateToLatest(key) # "updated", "upToDate" or "lowMatch"
```

`app.updateFromTemplate(key, path, allowMismatch=False)` merges a given file and returns the same
statuses, or `"templateMismatch"` when the file is not a version of the template. Failures
raise `RuntimeError`.

### From the command line

The same three-way merge runs outside Natron. With the version the shot was last merged with as
the base, and the key the template was imported under (the Template nodes of the base and the
new version are left out, as in Natron):

```
natron-merge merge --scope template:grade_tpl grade_v001.ntpz shot.ntpz grade_v002.ntpz -o shot-updated.ntpz
```

A shot can import several templates, or the same template twice under different keys; each
key is updated on its own, and the conflicts saved by earlier updates are kept.

## Merging on the node graph

Inside Natron, both a comparison and the conflicts of an update are shown and resolved on the
node graph itself.

### Comparing and picking

**File > Compare with File...** and **Compare with Template Base** (on a template node's
context menu) show how the project differs from another version: added nodes are tinted
green, changed nodes amber, moved nodes are outlined, and nodes the other version has but the
project does not are drawn as ghosts where they would sit. Ghosts can be selected like nodes:
click one, Shift- or Ctrl-click to add or remove one, or drag a rubber band over them. Selected
ghosts get the selection outline. Delete, copy and drag leave ghosts alone.

Right-click the selection for the picks that apply to it. Each pick is one undo step, and the
comparison is made again after it, and after it is undone or redone, so the graph always shows
what still differs. A pick applies only to the selected nodes it fits, so a mixed selection can
offer several.

- **Restore** (ghosts) recreates the node where its ghost is drawn, with its parameters, and
  wires it as the other version had it: an input comes from the matching live node, or from a
  ghost restored with it, and is left empty otherwise. Ghosts restored together are wired to
  each other, so a run of adjacent ghosts comes back as a chain. A removed Group comes back
  whole, with everything inside it. A restored node whose script name is taken gets a fresh one,
  and the expressions and inputs of the other restored nodes that name it follow.
- **Restore (Keep My Wiring)** and **Restore (Splice Everywhere)** restore the same way but
  settle rewired readers without asking (see below).
- **Revert to Other** (changed nodes) gives the node every changed parameter, input and
  appearance the other version has.
- **Remove** (added nodes) deletes the node and connects its main input to whatever it fed, as
  a normal delete does.
- **Restore Position** (moved nodes) moves the node back to where the other version has it.

### Splicing restored nodes back in

Restoring a node also reconnects the nodes that read from it in the other version. For each such
input in the project:

- an input that is empty, or still connected to what fed the removed node, is connected to the
  restored node;
- an input the artist connected to something else since is a choice. **Restore** leaves it and
  adds a pending splice choice on the reader to the merge conflicts, with **Keep My Wiring**
  (leave it) and **Splice In** (connect it to the restored node). **Restore (Keep My Wiring)**
  leaves it with no choice, and **Restore (Splice Everywhere)** connects it.

Pending splice choices are merge conflicts like any other: they carry the badge, appear in the
popover and the Merge Conflicts panel, and are saved with the project.

### The merge popover

Click the conflict badge of a node with merge conflicts (its tooltip says "Click to resolve")
to open the merge popover beside it. It lists each of the node's conflicts with a button for
each choice it can take, and **All Mine** and **All Theirs** for all of them. Each choice is
one undo step. While a comparison shows a node as changed, click the change marker on the node
to open the same popover with a **Changes** section: **Take** beside each changed parameter
gives it the other version's value, and **Revert Node** does what Revert to Other does.

The **Node Diff** panel, which lists what differs on the selected nodes, offers the same two
picks: **Revert Node** on the heading of each changed node, and **Take** on the first row of each
changed parameter. They are the same undo steps as in the popover. Revert Node is disabled on a
node that only moved; use Restore Position for that.

The popover is not modal. It refreshes after every choice, and closes on Escape, on a click
elsewhere, or once nothing on the node is left to choose.

Only choices that are possible and lead somewhere new are offered: a choice that would give
the same result as one listed before it is left out, and Keep Both Keys is offered only for
keys at different frames, since two keys cannot share a frame.

### Previews of structural conflicts

Input, insert-on-the-same-link, reparent, delete-modify and pending splice conflicts change the
wiring rather than a value. While the popover of a node with such a conflict is open, each side
is drawn faintly on the graph: dashed wires in the side's colour for its connections, the
deleted node of a delete-modify conflict as a ghost, and for an insert the side's nodes
outlined and the other side's dimmed. Hover a choice, in the popover or the Merge Conflicts
panel, to draw that side in full. Click a side's preview ghost or the end of its wire to take
that side. Conflicts reopened from a saved project keep their previews.

### The merge bar

A bar along the bottom of the node graph shows either the comparison or the conflicts, never
both, and is hidden when there is neither.

- During a comparison it names what is compared, counts the added, changed, removed and moved
  nodes, and offers **Previous Change**, **Next Change**, **Restore Selected**, **Revert
  Selected** and **Clear Comparison**.
- While the project has merge conflicts it counts them and offers **Previous Conflict**,
  **Next Conflict**, **All Mine**, **All Theirs** and **Open Panel**.

Starting a comparison hides the conflict badges until it is cleared; an update that raises
conflicts clears the comparison first. After such an update the graph frames the first node
with a conflict. The Merge Conflicts panel does not open by itself, after an update or when a
project with conflicts is opened; **Open Panel** shows it. It offers the same choices as the
popover.

The node graph's menu has the same navigation, which frames the node and opens its popover:

| Action            | Shortcut          |
|-------------------|-------------------|
| Next Conflict     | Alt+Down          |
| Previous Conflict | Alt+Up            |
| Next Change       | Alt+Shift+Down    |
| Previous Change   | Alt+Shift+Up      |

### Scripting

The GUI app (`app` in the script editor) has a call for every pick, each one undo step that
returns `False`, with the reason in the script editor, when it did nothing:

- `app.restoreFromComparison(nodeNames=[], splice="ask")` restores the ghosts named by script
  name or node ID, or the selected ghosts when the list is empty. `splice` is `"ask"`,
  `"keepMine"` or `"everywhere"`, as Restore, Restore (Keep My Wiring) and Restore (Splice
  Everywhere).
- `app.revertFromComparison(effects)`, `app.removeAddedFromComparison(effects)` and
  `app.restorePositionFromComparison(effects)` are Revert to Other, Remove and Restore Position.
- `app.takeKnobFromComparison(effect, knobName)` is Take on one parameter.
- `app.getComparisonGhosts()` lists the ghosts as dicts with `name`, `nodeId`, `group` (empty
  at the top level) and `selected`; `app.selectComparisonGhosts(nodeNames)` selects exactly the
  ghosts named.
- `app.openMergePopover(effect)` and `app.isMergePopoverOpen()` open and query the popover, and
  `app.mergeBarMode()` returns `"comparison"`, `"conflicts"` or `"none"`.

Conflicts are listed and resolved on the app, in the GUI or not:

- `app.getMergeConflicts()` returns each unresolved conflict as a dict with `index`, `node` (the
  fully qualified node name), `nodeId`, `kind`, `label`, and `choices` (`"mine"`, `"theirs"`,
  `"base"` or an option's name) with their labels in `choiceLabels`. A pending splice choice
  has kind `"splice-choice"`, with `"mine"` for Keep My Wiring and `"theirs"` for Splice In.
- `app.resolveMergeConflict(index, choice)` resolves one with a name from its `choices`, as one
  undo step in the GUI.
